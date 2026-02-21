/*
 * TAVRN v2: Topology Aware Vicinity-Reactive Network
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN routing protocol implementation — the central class that ties together
 * all sub-components: GTT (topology awareness), AODV-style reactive routing,
 * mentorship bootstrap, TC-UPDATE gossip, and piggybacked metadata.
 */
#define NS_LOG_APPEND_CONTEXT                                                                      \
    if (m_ipv4)                                                                                    \
    {                                                                                              \
        std::clog << "[node " << m_ipv4->GetObject<Node>()->GetId() << "] ";                       \
    }

#include "tavrn-routing-protocol.h"

#include "ns3/adhoc-wifi-mac.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/inet-socket-address.h"
#include "ns3/log.h"
#include "ns3/pointer.h"
#include "ns3/random-variable-stream.h"
#include "ns3/string.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/udp-header.h"
#include "ns3/udp-l4-protocol.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-net-device.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TavrnRoutingProtocol");

namespace tavrn
{

NS_OBJECT_ENSURE_REGISTERED(RoutingProtocol);

/// UDP port for TAVRN control traffic
const uint32_t RoutingProtocol::TAVRN_PORT = 655;

// ============================================================================
// DeferredRouteOutputTag — same pattern as AODV
// ============================================================================

/**
 * @ingroup tavrn
 * @brief Tag used by TAVRN to defer route output
 */
class DeferredRouteOutputTag : public Tag
{
  public:
    DeferredRouteOutputTag(int32_t o = -1)
        : Tag(),
          m_oif(o)
    {
    }

    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::tavrn::DeferredRouteOutputTag")
                                .SetParent<Tag>()
                                .SetGroupName("Tavrn")
                                .AddConstructor<DeferredRouteOutputTag>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override
    {
        return GetTypeId();
    }

    int32_t GetInterface() const
    {
        return m_oif;
    }

    void SetInterface(int32_t oif)
    {
        m_oif = oif;
    }

    uint32_t GetSerializedSize() const override
    {
        return sizeof(int32_t);
    }

    void Serialize(TagBuffer i) const override
    {
        i.WriteU32(m_oif);
    }

    void Deserialize(TagBuffer i) override
    {
        m_oif = i.ReadU32();
    }

    void Print(std::ostream& os) const override
    {
        os << "DeferredRouteOutputTag: output interface = " << m_oif;
    }

  private:
    int32_t m_oif;
};

NS_OBJECT_ENSURE_REGISTERED(DeferredRouteOutputTag);

// ============================================================================
// TavrnShimTag — marks data packets carrying piggybacked TopologyMetadataHeader
// ============================================================================

/**
 * @ingroup tavrn
 * @brief PacketTag indicating a TopologyMetadataHeader is prepended to the IP payload.
 *
 * When a TAVRN node forwards a data packet, it copies the packet, prepends
 * a TopologyMetadataHeader, and attaches this tag. The next-hop TAVRN node
 * detects the tag in RouteInput, strips the metadata header (processing it
 * for GTT updates), and passes the clean packet onward.
 *
 * This implements Principle 3: "Piggyback topology control on existing traffic."
 */
class TavrnShimTag : public Tag
{
  public:
    TavrnShimTag()
        : Tag(),
          m_prevHop(Ipv4Address())
    {
    }

    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::tavrn::TavrnShimTag")
                                .SetParent<Tag>()
                                .SetGroupName("Tavrn")
                                .AddConstructor<TavrnShimTag>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override
    {
        return GetTypeId();
    }

    /// Store the previous hop that piggybacked the metadata
    void SetPrevHop(Ipv4Address addr) { m_prevHop = addr; }
    Ipv4Address GetPrevHop() const { return m_prevHop; }

    uint32_t GetSerializedSize() const override
    {
        return 4; // 4 bytes for previous hop IPv4 address
    }

    void Serialize(TagBuffer i) const override
    {
        i.WriteU32(m_prevHop.Get());
    }

    void Deserialize(TagBuffer i) override
    {
        m_prevHop = Ipv4Address(i.ReadU32());
    }

    void Print(std::ostream& os) const override
    {
        os << "TavrnShimTag: prevHop=" << m_prevHop;
    }

  private:
    Ipv4Address m_prevHop; ///< IP address of the node that piggybacked the metadata
};

NS_OBJECT_ENSURE_REGISTERED(TavrnShimTag);

// ============================================================================
// Constructor / Destructor
// ============================================================================

RoutingProtocol::RoutingProtocol()
    : m_rreqRetries(2),
      m_ttlStart(1),
      m_ttlIncrement(2),
      m_ttlThreshold(7),
      m_rreqRateLimit(10),
      m_rerrRateLimit(10),
      m_activeRouteTimeout(Seconds(600)),
      m_netDiameter(35),
      m_nodeTraversalTime(MilliSeconds(40)),
      m_netTraversalTime(Time(2 * m_netDiameter * m_nodeTraversalTime)),
      m_pathDiscoveryTime(Time(2 * m_netTraversalTime)),
      m_myRouteTimeout(Time(2 * std::max(m_pathDiscoveryTime, m_activeRouteTimeout))),
      m_maxQueueLen(64),
      m_maxQueueTime(Seconds(30)),
      m_destinationOnly(false),
      m_gratuitousReply(true),
      m_timeoutBuffer(2),
      m_allowedHelloLoss(2),
      m_enableBroadcast(true),
      m_nextHopWait(MilliSeconds(50)),
      m_blackListTimeout(Time(m_rreqRetries * Time(2 * m_netDiameter * m_nodeTraversalTime))),
      m_gttTtl(Seconds(300)),
      m_softExpiryThreshold(0.5),
      m_enablePeriodicHello(true),
      m_helloInterval(Seconds(150)),
      m_syncPageSize(15),
      m_maxMetadataEntries(5),
      m_piggybackCooldown(Seconds(5)),
      m_tcUpdateExpiryWindow(Seconds(30)),
      m_gtt(Seconds(300), 0.5),
      m_routingTable(Seconds(600)),
      m_queue(64, Seconds(30)),
      m_rreqIdCache(Seconds(2)),
      m_tcUuidCache(Seconds(30)),        // separate UUID cache with longer lifetime
      m_nb(Seconds(600)),
      m_dpd(Seconds(2)),
      m_requestId(0),
      m_seqNo(0),
      m_tcUpdateSeqNo(0),
      m_rreqCount(0),
      m_rerrCount(0),
      m_isBootstrapped(false),
      m_syncNextIndex(0),
      m_syncTimer(Timer::CANCEL_ON_DESTROY),
      m_bootstrapTimer(Timer::CANCEL_ON_DESTROY),
      m_syncPullTimeout(Timer::CANCEL_ON_DESTROY),
      m_syncPullRetries(0),
      m_offerCollectionTimer(Timer::CANCEL_ON_DESTROY),
      m_metadataCursor(0),
      m_conditionalCursor(0),
      m_htimer(Timer::CANCEL_ON_DESTROY),
      m_gttMaintenanceTimer(Timer::CANCEL_ON_DESTROY),
      m_rreqRateLimitTimer(Timer::CANCEL_ON_DESTROY),
      m_rerrRateLimitTimer(Timer::CANCEL_ON_DESTROY)
{
    m_uniformRandomVariable = CreateObject<UniformRandomVariable>();
}

RoutingProtocol::~RoutingProtocol()
{
}

// ============================================================================
// GetTypeId — Register all ns-3 Attributes
// ============================================================================

TypeId
RoutingProtocol::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::tavrn::RoutingProtocol")
            .SetParent<Ipv4RoutingProtocol>()
            .SetGroupName("Tavrn")
            .AddConstructor<RoutingProtocol>()
            .AddAttribute("RreqRetries",
                          "Maximum number of retransmissions of RREQ to discover a route.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_rreqRetries),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RreqRateLimit",
                          "Maximum number of RREQ per second.",
                          UintegerValue(10),
                          MakeUintegerAccessor(&RoutingProtocol::m_rreqRateLimit),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("RerrRateLimit",
                          "Maximum number of RERR per second.",
                          UintegerValue(10),
                          MakeUintegerAccessor(&RoutingProtocol::m_rerrRateLimit),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("ActiveRouteTimeout",
                          "Period of time during which a route is considered valid. "
                          "Set to 2x GttTtl: routes outlive GTT entries so stale GTT "
                          "entries prefer vectored routing before falling back to floods.",
                          TimeValue(Seconds(600)),
                          MakeTimeAccessor(&RoutingProtocol::m_activeRouteTimeout),
                          MakeTimeChecker())
            .AddAttribute("NetDiameter",
                          "Maximum possible number of hops between two nodes.",
                          UintegerValue(35),
                          MakeUintegerAccessor(&RoutingProtocol::m_netDiameter),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("NodeTraversalTime",
                          "Conservative estimate of the average one-hop traversal time.",
                          TimeValue(MilliSeconds(40)),
                          MakeTimeAccessor(&RoutingProtocol::m_nodeTraversalTime),
                          MakeTimeChecker())
            .AddAttribute("MaxQueueLen",
                          "Maximum number of packets that the routing protocol may buffer.",
                          UintegerValue(64),
                          MakeUintegerAccessor(&RoutingProtocol::m_maxQueueLen),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("MaxQueueTime",
                          "Maximum time packets can be queued.",
                          TimeValue(Seconds(30)),
                          MakeTimeAccessor(&RoutingProtocol::m_maxQueueTime),
                          MakeTimeChecker())
            .AddAttribute("DestinationOnly",
                          "Indicates only the destination may respond to this RREQ.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RoutingProtocol::m_destinationOnly),
                          MakeBooleanChecker())
            .AddAttribute(
                "GratuitousReply",
                "Indicates whether a gratuitous RREP should be unicast to the originator.",
                BooleanValue(true),
                MakeBooleanAccessor(&RoutingProtocol::m_gratuitousReply),
                MakeBooleanChecker())
            .AddAttribute("GttTtl",
                          "GTT entry default time-to-live.",
                          TimeValue(Seconds(300)),
                          MakeTimeAccessor(&RoutingProtocol::m_gttTtl),
                          MakeTimeChecker())
            .AddAttribute("SoftExpiryThreshold",
                          "Fraction of GTT-TTL at which soft expiry triggers freshness request.",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&RoutingProtocol::m_softExpiryThreshold),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("EnablePeriodicHello",
                          "Whether periodic HELLO messages are enabled. "
                          "When true, tiny 10-byte HELLOs refresh 1-hop neighbor GTT entries.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::m_enablePeriodicHello),
                          MakeBooleanChecker())
            .AddAttribute("HelloInterval",
                          "Interval between periodic HELLO broadcasts. "
                          "Default: 0.5 * GttTtl (soft-expiry duty cycle).",
                          TimeValue(Seconds(150)),
                          MakeTimeAccessor(&RoutingProtocol::m_helloInterval),
                          MakeTimeChecker())
            .AddAttribute("SyncPageSize",
                          "Number of GTT entries per SYNC_DATA page.",
                          UintegerValue(15),
                          MakeUintegerAccessor(&RoutingProtocol::m_syncPageSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("MaxMetadataEntries",
                          "Maximum GTT metadata entries piggybacked per message.",
                          UintegerValue(5),
                          MakeUintegerAccessor(&RoutingProtocol::m_maxMetadataEntries),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("PiggybackCooldown",
                          "Cooldown period after piggybacking a GTT entry on a data packet. "
                          "Prevents redundant re-propagation of the same entry.",
                          TimeValue(Seconds(5)),
                          MakeTimeAccessor(&RoutingProtocol::m_piggybackCooldown),
                          MakeTimeChecker())
            .AddAttribute("TcUpdateExpiryWindow",
                          "UUID expiry window for TC-UPDATE deduplication.",
                          TimeValue(Seconds(30)),
                          MakeTimeAccessor(&RoutingProtocol::m_tcUpdateExpiryWindow),
                          MakeTimeChecker())
            // AODV-parity attributes
            .AddAttribute("TtlStart",
                          "Initial TTL value for RREQ.",
                          UintegerValue(1),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlStart),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlIncrement",
                          "TTL increment for each attempt using expanding ring search.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlIncrement),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlThreshold",
                          "Maximum TTL value for expanding ring search.",
                          UintegerValue(7),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlThreshold),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TimeoutBuffer",
                          "Provide an upper bound on the time for which an upstream node A "
                          "can have a valid route entry for route (A, B).",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_timeoutBuffer),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("AllowedHelloLoss",
                          "Number of hello messages which may be lost for valid link.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_allowedHelloLoss),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("EnableBroadcast",
                          "Indicates whether a broadcast data packets forwarding enable.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::m_enableBroadcast),
                          MakeBooleanChecker())
            .AddAttribute("NextHopWait",
                          "Period of our waiting for the neighbour's RREP_ACK.",
                          TimeValue(MilliSeconds(50)),
                          MakeTimeAccessor(&RoutingProtocol::m_nextHopWait),
                          MakeTimeChecker())
            .AddAttribute("BlackListTimeout",
                          "Time for which the node is put into the blacklist.",
                          TimeValue(Time(5.6 * 2 * MilliSeconds(40))),
                          MakeTimeAccessor(&RoutingProtocol::m_blackListTimeout),
                          MakeTimeChecker())
            .AddTraceSource("ControlOverhead",
                            "Fired on every control packet transmission (size in bytes).",
                            MakeTraceSourceAccessor(&RoutingProtocol::m_controlOverheadTrace),
                            "ns3::TracedValueCallback::Uint32")
            .AddTraceSource("TopologyConvergence",
                            "Fired when a topology change has fully propagated.",
                            MakeTraceSourceAccessor(&RoutingProtocol::m_convergenceTrace),
                            "ns3::tavrn::RoutingProtocol::ConvergenceTracedCallback")
            .AddTraceSource("RouteDiscovery",
                            "Fired when a route discovery completes.",
                            MakeTraceSourceAccessor(&RoutingProtocol::m_routeDiscoveryTrace),
                            "ns3::tavrn::RoutingProtocol::RouteDiscoveryTracedCallback");
    return tid;
}

// ============================================================================
// DoDispose / DoInitialize
// ============================================================================

void
RoutingProtocol::DoDispose()
{
    // Cancel all pending timers before disposing
    for (auto& pair : m_addressReqTimer)
    {
        pair.second.Cancel();
    }
    m_addressReqTimer.clear();
    m_bootstrapTimer.Cancel();
    m_syncPullTimeout.Cancel();
    m_offerCollectionTimer.Cancel();
    m_gttMaintenanceTimer.Cancel();
    m_syncTimer.Cancel();
    m_htimer.Cancel();
    // Cancel rate-limit timers
    m_rreqRateLimitTimer.Cancel();
    m_rerrRateLimitTimer.Cancel();
    // Cancel tracked m_recentlyMentored cleanup events
    for (auto& pair : m_recentlyMentored)
    {
        Simulator::Cancel(pair.second);
    }
    m_recentlyMentored.clear();
    // Cancel pending SYNC_OFFER events
    for (auto& pair : m_pendingSyncOfferEvents)
    {
        Simulator::Cancel(pair.second);
    }
    m_pendingSyncOfferEvents.clear();
    // Cancel pending RREP-ACK events
    for (auto& pair : m_pendingAckEvents)
    {
        Simulator::Cancel(pair.second);
    }
    m_pendingAckEvents.clear();

    m_ipv4 = nullptr;
    for (auto iter = m_socketAddresses.begin(); iter != m_socketAddresses.end(); ++iter)
    {
        iter->first->Close();
    }
    m_socketAddresses.clear();
    for (auto iter = m_socketSubnetBroadcastAddresses.begin();
         iter != m_socketSubnetBroadcastAddresses.end();
         ++iter)
    {
        iter->first->Close();
    }
    m_socketSubnetBroadcastAddresses.clear();
    Ipv4RoutingProtocol::DoDispose();
}

void
RoutingProtocol::DoInitialize()
{
    NS_LOG_FUNCTION(this);

    // Recompute derived timers from potentially-modified attributes
    m_netTraversalTime = Time(2 * m_netDiameter * m_nodeTraversalTime);
    m_pathDiscoveryTime = Time(2 * m_netTraversalTime);
    m_myRouteTimeout = Time(2 * std::max(m_pathDiscoveryTime, m_activeRouteTimeout));
    // Recompute blacklist timeout (matches AODV: RreqRetries * NetTraversalTime)
    m_blackListTimeout = Time(m_rreqRetries * m_netTraversalTime);
    m_nextHopWait = m_nodeTraversalTime + MilliSeconds(10);

    // Use m_pathDiscoveryTime for RREQ ID cache lifetime (matches AODV)
    m_rreqIdCache.SetLifetime(m_pathDiscoveryTime);

    // Wire TC-UPDATE UUID cache lifetime from the attribute
    m_tcUuidCache.SetLifetime(m_tcUpdateExpiryWindow);

    // Propagate queue attributes to the queue object
    m_queue.SetMaxQueueLen(m_maxQueueLen);
    m_queue.SetQueueTimeout(m_maxQueueTime);

    // Sync GTT parameters with attributes (in case they were changed via Config)
    m_gtt.SetDefaultTtl(m_gttTtl);
    m_gtt.SetSoftExpiryThreshold(m_softExpiryThreshold);

    if (m_enablePeriodicHello)
    {
        m_htimer.SetFunction(&RoutingProtocol::HelloTimerExpire, this);
        uint32_t startTime = m_uniformRandomVariable->GetInteger(0, 100);
        m_htimer.Schedule(MilliSeconds(startTime));
    }
    Ipv4RoutingProtocol::DoInitialize();
}

// ============================================================================
// Start — Open sockets, init timers, send initial HELLO
// ============================================================================

void
RoutingProtocol::Start()
{
    NS_LOG_FUNCTION(this);

    m_nb.SetCallback(MakeCallback(&RoutingProtocol::SendRerrWhenBreaksLinkToNextHop, this));
    m_nb.ScheduleTimer();

    m_rreqRateLimitTimer.SetFunction(&RoutingProtocol::RreqRateLimitTimerExpire, this);
    m_rreqRateLimitTimer.Schedule(Seconds(1));

    m_rerrRateLimitTimer.SetFunction(&RoutingProtocol::RerrRateLimitTimerExpire, this);
    m_rerrRateLimitTimer.Schedule(Seconds(1));

    // GTT maintenance timer: check for soft/hard expiry periodically
    m_gttMaintenanceTimer.SetFunction(&RoutingProtocol::GttMaintenanceTimerExpire, this);
    m_gttMaintenanceTimer.Schedule(Seconds(10));

    // Initial HELLO is now triggered in NotifyInterfaceUp when first
    // non-loopback interface comes up, ensuring sockets are ready.

    // Schedule bootstrap timeout — if no SYNC_OFFER arrives, self-bootstrap
    m_bootstrapTimer.SetFunction(&RoutingProtocol::BootstrapTimeoutExpire, this);
    m_bootstrapTimer.Schedule(Seconds(3));
}

// ============================================================================
// SetIpv4
// ============================================================================

void
RoutingProtocol::SetIpv4(Ptr<Ipv4> ipv4)
{
    NS_ASSERT(ipv4);
    NS_ASSERT(!m_ipv4);

    m_ipv4 = ipv4;

    // Create lo route. It is asserted that the only one interface up for now is loopback
    NS_ASSERT(m_ipv4->GetNInterfaces() == 1 &&
              m_ipv4->GetAddress(0, 0).GetLocal() == Ipv4Address("127.0.0.1"));
    m_lo = m_ipv4->GetNetDevice(0);
    NS_ASSERT(m_lo);

    // Add loopback route
    RoutingTableEntry rt(
        /*dev=*/m_lo,
        /*dst=*/Ipv4Address::GetLoopback(),
        /*vSeqNo=*/true,
        /*seqNo=*/0,
        /*iface=*/Ipv4InterfaceAddress(Ipv4Address::GetLoopback(), Ipv4Mask("255.0.0.0")),
        /*hops=*/1,
        /*nextHop=*/Ipv4Address::GetLoopback(),
        /*lifetime=*/Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);

    Simulator::ScheduleNow(&RoutingProtocol::Start, this);
}

// ============================================================================
// NotifyInterfaceUp
// ============================================================================

void
RoutingProtocol::NotifyInterfaceUp(uint32_t i)
{
    NS_LOG_FUNCTION(this << m_ipv4->GetAddress(i, 0).GetLocal());
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (l3->GetNAddresses(i) > 1)
    {
        NS_LOG_WARN("TAVRN does not work with more than one address per each interface.");
    }
    Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
    if (iface.GetLocal() == Ipv4Address("127.0.0.1"))
    {
        return;
    }

    // Create a socket to listen only on this interface
    Ptr<Socket> socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    NS_ASSERT(socket);
    socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvTavrn, this));
    socket->BindToNetDevice(l3->GetNetDevice(i));
    socket->Bind(InetSocketAddress(iface.GetLocal(), TAVRN_PORT));
    socket->SetAllowBroadcast(true);
    socket->SetIpRecvTtl(true);
    m_socketAddresses.insert(std::make_pair(socket, iface));

    // Create also a subnet broadcast socket
    socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    NS_ASSERT(socket);
    socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvTavrn, this));
    socket->BindToNetDevice(l3->GetNetDevice(i));
    socket->Bind(InetSocketAddress(iface.GetBroadcast(), TAVRN_PORT));
    socket->SetAllowBroadcast(true);
    socket->SetIpRecvTtl(true);
    m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

    // Add local broadcast record to the routing table
    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
    RoutingTableEntry rt(/*dev=*/dev,
                         /*dst=*/iface.GetBroadcast(),
                         /*vSeqNo=*/true,
                         /*seqNo=*/0,
                         /*iface=*/iface,
                         /*hops=*/1,
                         /*nextHop=*/iface.GetBroadcast(),
                         /*lifetime=*/Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);

    if (l3->GetInterface(i)->GetArpCache())
    {
        m_nb.AddArpCache(l3->GetInterface(i)->GetArpCache());
    }

    // Allow neighbor manager use this interface for layer 2 feedback if possible
    Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice>();
    if (!wifi)
    {
        return;
    }
    Ptr<WifiMac> mac = wifi->GetMac();
    if (!mac)
    {
        return;
    }

    mac->TraceConnectWithoutContext("DroppedMpdu",
                                     MakeCallback(&RoutingProtocol::NotifyTxError, this));

    // Connect to MonitorSnifferRx for real RSSI measurement
    Ptr<WifiPhy> phy = wifi->GetPhy();
    if (phy)
    {
        phy->TraceConnectWithoutContext(
            "MonitorSnifferRx",
            MakeCallback(&RoutingProtocol::RecvPhyRxSniffer, this));
    }

    // Trigger initial HELLO on first real (non-loopback) interface up
    if (m_socketAddresses.size() == 1)
    {
        // Add self to GTT
        m_gtt.AddOrUpdateEntry(iface.GetLocal(), m_seqNo, 0);
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 100)),
                            &RoutingProtocol::SendHello,
                            this);
    }
}

// ============================================================================
// NotifyInterfaceDown
// ============================================================================

void
RoutingProtocol::NotifyInterfaceDown(uint32_t i)
{
    NS_LOG_FUNCTION(this << m_ipv4->GetAddress(i, 0).GetLocal());

    // Disable layer 2 link state monitoring (if possible)
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    Ptr<NetDevice> dev = l3->GetNetDevice(i);
    Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice>();
    if (wifi)
    {
        Ptr<WifiMac> mac = wifi->GetMac()->GetObject<AdhocWifiMac>();
        if (mac)
        {
            mac->TraceDisconnectWithoutContext(
                "DroppedMpdu",
                MakeCallback(&RoutingProtocol::NotifyTxError, this));
            m_nb.DelArpCache(l3->GetInterface(i)->GetArpCache());
        }
        // Disconnect MonitorSnifferRx
        Ptr<WifiPhy> phy = wifi->GetPhy();
        if (phy)
        {
            phy->TraceDisconnectWithoutContext(
                "MonitorSnifferRx",
                MakeCallback(&RoutingProtocol::RecvPhyRxSniffer, this));
        }
    }

    // Close socket
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    NS_ASSERT(socket);
    socket->Close();
    m_socketAddresses.erase(socket);

    // Close broadcast socket
    socket = FindSubnetBroadcastSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    NS_ASSERT(socket);
    socket->Close();
    m_socketSubnetBroadcastAddresses.erase(socket);

    if (m_socketAddresses.empty())
    {
        NS_LOG_LOGIC("No TAVRN interfaces");
        m_htimer.Cancel();
        m_nb.Clear();
        m_routingTable.Clear();
        return;
    }
    m_routingTable.DeleteAllRoutesFromInterface(m_ipv4->GetAddress(i, 0));
}

// ============================================================================
// NotifyAddAddress
// ============================================================================

void
RoutingProtocol::NotifyAddAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    NS_LOG_FUNCTION(this << " interface " << i << " address " << address);
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (!l3->IsUp(i))
    {
        return;
    }
    if (l3->GetNAddresses(i) == 1)
    {
        Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(iface);
        if (!socket)
        {
            if (iface.GetLocal() == Ipv4Address("127.0.0.1"))
            {
                return;
            }
            // Create a socket to listen only on this interface
            Ptr<Socket> socket =
                Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvTavrn, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetLocal(), TAVRN_PORT));
            socket->SetAllowBroadcast(true);
            m_socketAddresses.insert(std::make_pair(socket, iface));

            // Create also a subnet directed broadcast socket
            socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvTavrn, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetBroadcast(), TAVRN_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

            // Add local broadcast record to the routing table
            Ptr<NetDevice> dev =
                m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
            RoutingTableEntry rt(/*dev=*/dev,
                                 /*dst=*/iface.GetBroadcast(),
                                 /*vSeqNo=*/true,
                                 /*seqNo=*/0,
                                 /*iface=*/iface,
                                 /*hops=*/1,
                                 /*nextHop=*/iface.GetBroadcast(),
                                 /*lifetime=*/Simulator::GetMaximumSimulationTime());
            m_routingTable.AddRoute(rt);
        }
    }
    else
    {
        NS_LOG_LOGIC(
            "TAVRN does not work with more than one address per each interface. Ignore.");
    }
}

// ============================================================================
// NotifyRemoveAddress
// ============================================================================

void
RoutingProtocol::NotifyRemoveAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    NS_LOG_FUNCTION(this);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(address);
    if (socket)
    {
        m_routingTable.DeleteAllRoutesFromInterface(address);
        socket->Close();
        m_socketAddresses.erase(socket);

        Ptr<Socket> unicastSocket = FindSubnetBroadcastSocketWithInterfaceAddress(address);
        if (unicastSocket)
        {
            unicastSocket->Close();
            m_socketSubnetBroadcastAddresses.erase(unicastSocket);
        }

        Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
        if (l3->GetNAddresses(i))
        {
            Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
            // Create a socket to listen only on this interface
            Ptr<Socket> socket =
                Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvTavrn, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetLocal(), TAVRN_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketAddresses.insert(std::make_pair(socket, iface));

            // Create also a broadcast socket
            socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvTavrn, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetBroadcast(), TAVRN_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

            // Add local broadcast record to the routing table
            Ptr<NetDevice> dev =
                m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
            RoutingTableEntry rt(/*dev=*/dev,
                                 /*dst=*/iface.GetBroadcast(),
                                 /*vSeqNo=*/true,
                                 /*seqNo=*/0,
                                 /*iface=*/iface,
                                 /*hops=*/1,
                                 /*nextHop=*/iface.GetBroadcast(),
                                 /*lifetime=*/Simulator::GetMaximumSimulationTime());
            m_routingTable.AddRoute(rt);
        }
        if (m_socketAddresses.empty())
        {
            NS_LOG_LOGIC("No TAVRN interfaces");
            m_htimer.Cancel();
            m_nb.Clear();
            m_routingTable.Clear();
            return;
        }
    }
    else
    {
        NS_LOG_LOGIC("Remove address not participating in TAVRN operation");
    }
}

// ============================================================================
// RouteOutput
// ============================================================================

Ptr<Ipv4Route>
RoutingProtocol::RouteOutput(Ptr<Packet> p,
                             const Ipv4Header& header,
                             Ptr<NetDevice> oif,
                             Socket::SocketErrno& sockerr)
{
    NS_LOG_FUNCTION(this << header << (oif ? oif->GetIfIndex() : 0));
    if (!p)
    {
        NS_LOG_DEBUG("Packet is == 0");
        return LoopbackRoute(header, oif);
    }
    if (m_socketAddresses.empty())
    {
        sockerr = Socket::ERROR_NOROUTETOHOST;
        NS_LOG_LOGIC("No TAVRN interfaces");
        Ptr<Ipv4Route> route;
        return route;
    }
    sockerr = Socket::ERROR_NOTERROR;
    Ptr<Ipv4Route> route;
    Ipv4Address dst = header.GetDestination();
    RoutingTableEntry rt;
    if (m_routingTable.LookupValidRoute(dst, rt))
    {
        route = rt.GetRoute();
        NS_ASSERT(route);
        NS_LOG_DEBUG("Exist route to " << route->GetDestination() << " from interface "
                                       << route->GetSource());
        if (oif && route->GetOutputDevice() != oif)
        {
            NS_LOG_DEBUG("Output device doesn't match. Dropped.");
            sockerr = Socket::ERROR_NOROUTETOHOST;
            return Ptr<Ipv4Route>();
        }
        UpdateRouteLifeTime(dst, m_activeRouteTimeout);
        UpdateRouteLifeTime(route->GetGateway(), m_activeRouteTimeout);
        return route;
    }

    // Valid route not found — return loopback, defer actual route request
    uint32_t iif = (oif ? m_ipv4->GetInterfaceForDevice(oif) : -1);
    DeferredRouteOutputTag tag(iif);
    NS_LOG_DEBUG("Valid Route not found");
    if (!p->PeekPacketTag(tag))
    {
        p->AddPacketTag(tag);
    }
    return LoopbackRoute(header, oif);
}

// ============================================================================
// RouteInput
// ============================================================================

bool
RoutingProtocol::RouteInput(Ptr<const Packet> p,
                            const Ipv4Header& header,
                            Ptr<const NetDevice> idev,
                            const UnicastForwardCallback& ucb,
                            const MulticastForwardCallback& mcb,
                            const LocalDeliverCallback& lcb,
                            const ErrorCallback& ecb)
{
    NS_LOG_FUNCTION(this << p->GetUid() << header.GetDestination() << idev->GetAddress());
    if (m_socketAddresses.empty())
    {
        NS_LOG_LOGIC("No TAVRN interfaces");
        return false;
    }
    NS_ASSERT(m_ipv4);
    NS_ASSERT(p);
    NS_ASSERT(m_ipv4->GetInterfaceForDevice(idev) >= 0);
    int32_t iif = m_ipv4->GetInterfaceForDevice(idev);

    Ipv4Address dst = header.GetDestination();
    Ipv4Address origin = header.GetSource();

    // Deferred route request
    if (idev == m_lo)
    {
        DeferredRouteOutputTag tag;
        if (p->PeekPacketTag(tag))
        {
            DeferredRouteOutput(p, header, ucb, ecb);
            return true;
        }
    }

    // True piggybacking: strip TavrnShimTag + TopologyMetadataHeader if present.
    // Previous hop's Forwarding() prepended metadata; we consume it here.
    // Use the shim tag's prevHop (the actual forwarder) instead of
    // origin (the flow source) to attribute metadata correctly on multi-hop paths.
    Ipv4Address prevHop;  // declared outside shim block for use below
    TavrnShimTag shimTag;
    if (p->PeekPacketTag(shimTag))
    {
        Ptr<Packet> clean = p->Copy();
        clean->RemovePacketTag(shimTag);
        TopologyMetadataHeader meta;
        clean->RemoveHeader(meta);

        prevHop = shimTag.GetPrevHop();
        if (prevHop == Ipv4Address())
        {
            prevHop = origin; // fallback if tag wasn't populated
        }

        if (meta.GetEntryCount() > 0)
        {
            ProcessTopologyMetadata(meta, prevHop);
        }

        // Continue with clean packet for all downstream paths
        p = clean;
    }
    else
    {
        // For packets WITHOUT shim tag (v2.1 clean data packets),
        // determine previous hop from the routing table reverse route.
        // If origin is 1-hop away, origin IS the previous hop.
        // Otherwise, use the next-hop from our route to the origin.
        RoutingTableEntry toOrigin;
        if (m_routingTable.LookupRoute(origin, toOrigin))
        {
            prevHop = toOrigin.GetNextHop();
        }
        else
        {
            // No route to origin — best guess is that origin itself sent it
            prevHop = origin;
        }
    }

    // Improvement 3: Passive learning — previous hop is alive at 1 hop
    // This now fires for ALL data packets (with or without shim tag).
    // On stable networks, v2.1 sends clean packets without metadata, but we still
    // learn that the previous forwarder is alive.
    if (prevHop != Ipv4Address() && !IsMyOwnAddress(prevHop))
    {
        GttEntry prevEntry;
        if (m_gtt.LookupEntry(prevHop, prevEntry))
        {
            m_gtt.RefreshEntry(prevHop, prevEntry.seqNo);
        }
        else
        {
            m_gtt.AddOrUpdateEntry(prevHop, 0, 1);
        }
    }

    // Improvement 3: Passive learning — source IP is alive (from IP header)
    if (!IsMyOwnAddress(origin))
    {
        GttEntry originEntry;
        if (m_gtt.LookupEntry(origin, originEntry))
        {
            m_gtt.RefreshEntry(origin, originEntry.seqNo);
        }
        else
        {
            m_gtt.AddOrUpdateEntry(origin, 0, 0); // hop count unknown for multi-hop source
        }
    }

    // Duplicate of own packet
    if (IsMyOwnAddress(origin))
    {
        return true;
    }

    // TAVRN is not a multicast routing protocol
    if (dst.IsMulticast())
    {
        return false;
    }

    // Broadcast local delivery/forwarding
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ipv4InterfaceAddress iface = j->second;
        if (m_ipv4->GetInterfaceForAddress(iface.GetLocal()) == iif)
        {
            if (dst == iface.GetBroadcast() || dst.IsBroadcast())
            {
                UpdateRouteLifeTime(origin, m_activeRouteTimeout);
                Ptr<Packet> packet = p->Copy();
                if (!lcb.IsNull())
                {
                    NS_LOG_LOGIC("Broadcast local delivery to " << iface.GetLocal());
                    lcb(p, header, iif);
                }
                else
                {
                    NS_LOG_ERROR("Unable to deliver packet locally due to null callback "
                                 << p->GetUid() << " from " << origin);
                    ecb(p, header, Socket::ERROR_NOROUTETOHOST);
                }
                if (header.GetProtocol() == UdpL4Protocol::PROT_NUMBER)
                {
                    UdpHeader udpHeader;
                    p->PeekHeader(udpHeader);
                    if (udpHeader.GetDestinationPort() == TAVRN_PORT)
                    {
                        // TAVRN packets sent in broadcast are already managed
                        return true;
                    }
                }
                if (header.GetTtl() > 1)
                {
                    NS_LOG_LOGIC("Forward broadcast. TTL " << (uint16_t)header.GetTtl());
                    // Suppress duplicate broadcast forwarding (mirrors AODV m_dpd)
                    if (m_dpd.IsDuplicate(packet, header))
                    {
                        NS_LOG_DEBUG("Suppressing duplicate broadcast " << packet->GetUid());
                    }
                    else
                    {
                        RoutingTableEntry toBroadcast;
                        if (m_routingTable.LookupRoute(dst, toBroadcast))
                        {
                            Ptr<Ipv4Route> route = toBroadcast.GetRoute();
                            ucb(route, packet, header);
                        }
                        else
                        {
                            NS_LOG_DEBUG("No route to forward broadcast. Drop packet "
                                         << p->GetUid());
                        }
                    }
                }
                else
                {
                    NS_LOG_DEBUG("TTL exceeded. Drop packet " << p->GetUid());
                }
                return true;
            }
        }
    }

    // Unicast local delivery
    if (m_ipv4->IsDestinationAddress(dst, iif))
    {
        UpdateRouteLifeTime(origin, m_activeRouteTimeout);
        RoutingTableEntry toOrigin;
        if (m_routingTable.LookupValidRoute(origin, toOrigin))
        {
            UpdateRouteLifeTime(toOrigin.GetNextHop(), m_activeRouteTimeout);
            m_nb.Update(toOrigin.GetNextHop(), m_activeRouteTimeout);
        }
        if (!lcb.IsNull())
        {
            NS_LOG_LOGIC("Unicast local delivery to " << dst);
            lcb(p, header, iif);
        }
        else
        {
            NS_LOG_ERROR("Unable to deliver packet locally due to null callback "
                         << p->GetUid() << " from " << origin);
            ecb(p, header, Socket::ERROR_NOROUTETOHOST);
        }
        return true;
    }

    // Check if input device supports IP forwarding
    if (!m_ipv4->IsForwarding(iif))
    {
        NS_LOG_LOGIC("Forwarding disabled for this interface");
        ecb(p, header, Socket::ERROR_NOROUTETOHOST);
        return true;
    }

    // Forwarding
    return Forwarding(p, header, ucb, ecb);
}

// ============================================================================
// DeferredRouteOutput
// ============================================================================

void
RoutingProtocol::DeferredRouteOutput(Ptr<const Packet> p,
                                     const Ipv4Header& header,
                                     UnicastForwardCallback ucb,
                                     ErrorCallback ecb)
{
    NS_LOG_FUNCTION(this << p << header);
    NS_ASSERT(p && p != Ptr<Packet>());

    QueueEntry newEntry(p, header, ucb, ecb);
    bool result = m_queue.Enqueue(newEntry);
    if (result)
    {
        NS_LOG_LOGIC("Add packet " << p->GetUid() << " to queue. Protocol "
                                   << (uint16_t)header.GetProtocol());
        RoutingTableEntry rt;
        bool result = m_routingTable.LookupRoute(header.GetDestination(), rt);
        if (!result || ((rt.GetFlag() != IN_SEARCH) && result))
        {
            NS_LOG_LOGIC("Send new RREQ for outbound packet to " << header.GetDestination());
            SendRequest(header.GetDestination());
        }
    }
}

// ============================================================================
// Forwarding
// ============================================================================

bool
RoutingProtocol::Forwarding(Ptr<const Packet> p,
                            const Ipv4Header& header,
                            UnicastForwardCallback ucb,
                            ErrorCallback ecb)
{
    NS_LOG_FUNCTION(this);
    Ipv4Address dst = header.GetDestination();
    Ipv4Address origin = header.GetSource();
    m_routingTable.Purge();
    RoutingTableEntry toDst;
    if (m_routingTable.LookupRoute(dst, toDst))
    {
        if (toDst.GetFlag() == VALID)
        {
            Ptr<Ipv4Route> route = toDst.GetRoute();
            NS_LOG_LOGIC(route->GetSource() << " forwarding to " << dst << " from " << origin
                                            << " packet " << p->GetUid());

            UpdateRouteLifeTime(origin, m_activeRouteTimeout);
            UpdateRouteLifeTime(dst, m_activeRouteTimeout);
            UpdateRouteLifeTime(route->GetGateway(), m_activeRouteTimeout);

            RoutingTableEntry toOrigin;
            m_routingTable.LookupRoute(origin, toOrigin);
            UpdateRouteLifeTime(toOrigin.GetNextHop(), m_activeRouteTimeout);

            m_nb.Update(route->GetGateway(), m_activeRouteTimeout);
            m_nb.Update(toOrigin.GetNextHop(), m_activeRouteTimeout);

            // TAVRN extension: Refresh GTT entries on forwarding (extend TTL without
            // changing seqNo). use RefreshEntry to avoid seqNo=0 rejection.
            // use toOrigin.GetHop() for origin, toDst.GetHop() for dst.
            GttEntry originEntry;
            if (m_gtt.LookupEntry(origin, originEntry))
            {
                m_gtt.RefreshEntry(origin, originEntry.seqNo);
            }
            else
            {
                m_gtt.AddOrUpdateEntry(origin, 0, toOrigin.GetHop());
            }
            GttEntry dstEntry;
            if (m_gtt.LookupEntry(dst, dstEntry))
            {
                m_gtt.RefreshEntry(dst, dstEntry.seqNo);
            }
            else
            {
                m_gtt.AddOrUpdateEntry(dst, 0, toDst.GetHop());
            }

            // Principle 3: Piggyback topology metadata on data forwarding.
            // Improvement 1: Only piggyback when there are soft-expired entries
            // needing propagation. Clean packets when topology is stable.
            // Store this node's address in the shim tag so the next hop
            // attributes metadata to the correct previous hop (not the flow origin).
            {
                TopologyMetadataHeader meta = BuildConditionalMetadata();
                if (meta.GetEntryCount() > 0)
                {
                    // Mode B: soft-expired entries need propagation
                    Ptr<Packet> shimmed = p->Copy();
                    shimmed->AddHeader(meta);
                    TavrnShimTag shimTag;
                    shimTag.SetPrevHop(route->GetSource());
                    shimmed->AddPacketTag(shimTag);
                    ucb(route, shimmed, header);
                }
                else
                {
                    // Mode A: topology stable — forward clean packet (AODV-sized)
                    ucb(route, p, header);
                }
            }

            return true;
        }
        else
        {
            if (toDst.GetValidSeqNo())
            {
                SendRerrWhenNoRouteToForward(dst, toDst.GetSeqNo(), origin);
                NS_LOG_DEBUG("Drop packet " << p->GetUid()
                                            << " because no route to forward it.");
                return false;
            }
        }
    }
    NS_LOG_LOGIC("route not found to " << dst << ". Send RERR message.");
    NS_LOG_DEBUG("Drop packet " << p->GetUid() << " because no route to forward it.");
    SendRerrWhenNoRouteToForward(dst, 0, origin);
    return false;
}

// ============================================================================
// RecvTavrn — Main receive dispatcher
// ============================================================================

void
RoutingProtocol::RecvTavrn(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Address sourceAddress;
    Ptr<Packet> packet = socket->RecvFrom(sourceAddress);
    InetSocketAddress inetSourceAddr = InetSocketAddress::ConvertFrom(sourceAddress);
    Ipv4Address sender = inetSourceAddr.GetIpv4();
    Ipv4Address receiver;

    if (m_socketAddresses.find(socket) != m_socketAddresses.end())
    {
        receiver = m_socketAddresses[socket].GetLocal();
    }
    else if (m_socketSubnetBroadcastAddresses.find(socket) !=
             m_socketSubnetBroadcastAddresses.end())
    {
        receiver = m_socketSubnetBroadcastAddresses[socket].GetLocal();
    }
    else
    {
        NS_ASSERT_MSG(false, "Received a packet from an unknown socket");
    }
    NS_LOG_DEBUG("TAVRN node " << this << " received a TAVRN packet from " << sender << " to "
                               << receiver);

    UpdateRouteToNeighbor(sender, receiver);

    TypeHeader tHeader(TAVRN_E_RREQ);
    packet->RemoveHeader(tHeader);
    if (!tHeader.IsValid())
    {
        NS_LOG_DEBUG("TAVRN message " << packet->GetUid() << " with unknown type received: "
                                      << tHeader.Get() << ". Drop");
        return;
    }

    switch (tHeader.Get())
    {
    case TAVRN_E_RREQ: {
        RecvRequest(packet, receiver, sender);
        break;
    }
    case TAVRN_E_RREP: {
        RecvReply(packet, receiver, sender);
        break;
    }
    case TAVRN_E_RERR: {
        RecvError(packet, sender);
        break;
    }
    case TAVRN_HELLO: {
        RecvHello(packet, sender);
        break;
    }
    case TAVRN_SYNC_OFFER: {
        RecvSyncOffer(packet, sender);
        break;
    }
    case TAVRN_SYNC_PULL: {
        RecvSyncPull(packet, sender);
        break;
    }
    case TAVRN_SYNC_DATA: {
        RecvSyncData(packet, sender);
        break;
    }
    case TAVRN_TC_UPDATE: {
        RecvTcUpdate(packet, sender);
        break;
    }
    case TAVRN_E_RREP_ACK: {
        RecvReplyAck(sender);
        break;
    }
    }
}

// ============================================================================
// RecvRequest — Process E_RREQ
// ============================================================================

void
RoutingProtocol::RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src)
{
    NS_LOG_FUNCTION(this);
    ERreqHeader rreqHeader;
    p->RemoveHeader(rreqHeader);

    // Remove piggybacked topology metadata
    TopologyMetadataHeader metaHeader;
    p->RemoveHeader(metaHeader);
    ProcessTopologyMetadata(metaHeader, src);

    // A node ignores all RREQs received from any node in its blacklist
    RoutingTableEntry toPrev;
    if (m_routingTable.LookupRoute(src, toPrev))
    {
        if (toPrev.IsUnidirectional())
        {
            NS_LOG_DEBUG("Ignoring RREQ from node in blacklist");
            return;
        }
    }

    uint32_t id = rreqHeader.GetId();
    Ipv4Address origin = rreqHeader.GetOrigin();

    // Update GTT for the originator
    m_gtt.AddOrUpdateEntry(origin, rreqHeader.GetOriginSeqno(), rreqHeader.GetHopCount());

    // Improvement 3: Passive learning — the sender (previous hop) is alive at 1 hop
    {
        GttEntry senderEntry;
        if (m_gtt.LookupEntry(src, senderEntry))
        {
            m_gtt.RefreshEntry(src, senderEntry.seqNo);
        }
        else
        {
            m_gtt.AddOrUpdateEntry(src, 0, 1);
        }
    }

    // Duplicate check
    if (m_rreqIdCache.IsDuplicate(origin, id))
    {
        NS_LOG_DEBUG("Ignoring RREQ due to duplicate");
        return;
    }

    // Increment RREQ hop count
    uint8_t hop = rreqHeader.GetHopCount() + 1;
    rreqHeader.SetHopCount(hop);

    // Create or update reverse route to originator
    RoutingTableEntry toOrigin;
    if (!m_routingTable.LookupRoute(origin, toOrigin))
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(
            /*dev=*/dev,
            /*dst=*/origin,
            /*vSeqNo=*/true,
            /*seqNo=*/rreqHeader.GetOriginSeqno(),
            /*iface=*/m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
            /*hops=*/hop,
            /*nextHop=*/src,
            /*lifetime=*/Time(2 * m_netTraversalTime - 2 * hop * m_nodeTraversalTime));
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        if (toOrigin.GetValidSeqNo())
        {
            if (int32_t(rreqHeader.GetOriginSeqno()) - int32_t(toOrigin.GetSeqNo()) > 0)
            {
                toOrigin.SetSeqNo(rreqHeader.GetOriginSeqno());
            }
        }
        else
        {
            toOrigin.SetSeqNo(rreqHeader.GetOriginSeqno());
        }
        toOrigin.SetValidSeqNo(true);
        toOrigin.SetNextHop(src);
        toOrigin.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toOrigin.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toOrigin.SetHop(hop);
        toOrigin.SetLifeTime(std::max(Time(2 * m_netTraversalTime - 2 * hop * m_nodeTraversalTime),
                                      toOrigin.GetLifeTime()));
        m_routingTable.Update(toOrigin);
    }

    // Update neighbor entry
    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(src, toNeighbor))
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev,
                                   src,
                                   false,
                                   rreqHeader.GetOriginSeqno(),
                                   m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                                   1,
                                   src,
                                   m_activeRouteTimeout);
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        toNeighbor.SetLifeTime(m_activeRouteTimeout);
        toNeighbor.SetValidSeqNo(false);
        toNeighbor.SetSeqNo(rreqHeader.GetOriginSeqno());
        toNeighbor.SetFlag(VALID);
        toNeighbor.SetOutputDevice(
            m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toNeighbor.SetInterface(
            m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toNeighbor.SetHop(1);
        toNeighbor.SetNextHop(src);
        m_routingTable.Update(toNeighbor);
    }
    m_nb.Update(src, m_activeRouteTimeout);

    NS_LOG_LOGIC(receiver << " receive RREQ with hop count "
                          << static_cast<uint32_t>(rreqHeader.GetHopCount()) << " ID "
                          << rreqHeader.GetId() << " to destination " << rreqHeader.GetDst());

    // Check if we are the destination
    if (IsMyOwnAddress(rreqHeader.GetDst()))
    {
        m_routingTable.LookupRoute(origin, toOrigin);
        NS_LOG_DEBUG("Send reply since I am the destination");
        SendReply(rreqHeader, toOrigin);
        return;
    }

    // Check if we have a valid cached route
    RoutingTableEntry toDst;
    Ipv4Address dst = rreqHeader.GetDst();
    if (m_routingTable.LookupRoute(dst, toDst))
    {
        if (toDst.GetNextHop() == src)
        {
            NS_LOG_DEBUG("Drop RREQ from " << src << ", dest next hop " << toDst.GetNextHop());
            return;
        }
        if ((rreqHeader.GetUnknownSeqno() ||
             (int32_t(toDst.GetSeqNo()) - int32_t(rreqHeader.GetDstSeqno()) >= 0)) &&
            toDst.GetValidSeqNo())
        {
            if (!rreqHeader.GetDestinationOnly() && toDst.GetFlag() == VALID)
            {
                m_routingTable.LookupRoute(origin, toOrigin);
                SendReplyByIntermediateNode(toDst, toOrigin, rreqHeader.GetGratuitousRrep());
                return;
            }
            rreqHeader.SetDstSeqno(toDst.GetSeqNo());
            rreqHeader.SetUnknownSeqno(false);
        }
    }

    // TTL check
    SocketIpTtlTag tag;
    p->RemovePacketTag(tag);
    if (tag.GetTtl() < 2)
    {
        NS_LOG_DEBUG("TTL exceeded. Drop RREQ origin " << src << " destination " << dst);
        return;
    }

    // GTT-Assisted Unicast Intercept: If the RREQ destination is a confirmed
    // 1-hop neighbor with a fresh entry (expire time > 0), unicast the RREQ
    // directly to the destination instead of broadcasting. The destination
    // will recognise itself as the target and respond with RREP. This eliminates
    // an entire broadcast wave for the last hop.
    //
    // Only intercept when neighbor entry is fresh (positive expire time
    // remaining). Stale entries are skipped — fall through to normal broadcast.
    if (m_nb.IsNeighbor(dst) && m_nb.GetExpireTime(dst) > Seconds(0))
    {
        NS_LOG_DEBUG("Unicast Intercept: dst=" << dst << " is a confirmed fresh neighbor, "
                     << "unicasting RREQ directly");
        for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
        {
            Ptr<Socket> socket = j->first;
            Ptr<Packet> packet = Create<Packet>();
            SocketIpTtlTag ttl;
            ttl.SetTtl(tag.GetTtl() - 1);
            packet->AddPacketTag(ttl);

            TopologyMetadataHeader newMeta = BuildTopologyMetadata();
            packet->AddHeader(newMeta);
            packet->AddHeader(rreqHeader);
            TypeHeader tHeader(TAVRN_E_RREQ);
            packet->AddHeader(tHeader);

            m_controlOverheadTrace(packet->GetSize());
            Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                                &RoutingProtocol::SendTo,
                                this,
                                socket,
                                packet,
                                dst);
        }
        return;
    }

    // Forward (rebroadcast) RREQ with piggybacked metadata
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag ttl;
        ttl.SetTtl(tag.GetTtl() - 1);
        packet->AddPacketTag(ttl);

        // Build and add piggybacked metadata
        TopologyMetadataHeader newMeta = BuildTopologyMetadata();
        packet->AddHeader(newMeta);
        packet->AddHeader(rreqHeader);
        TypeHeader tHeader(TAVRN_E_RREQ);
        packet->AddHeader(tHeader);

        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        m_lastBcastTime = Simulator::Now();
        m_controlOverheadTrace(packet->GetSize());
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            packet,
                            destination);
    }
}

// ============================================================================
// RecvReply — Process E_RREP
// ============================================================================

void
RoutingProtocol::RecvReply(Ptr<Packet> p, Ipv4Address my, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << " src " << src);
    ERrepHeader rrepHeader;
    p->RemoveHeader(rrepHeader);

    // Remove piggybacked topology metadata
    TopologyMetadataHeader metaHeader;
    p->RemoveHeader(metaHeader);
    ProcessTopologyMetadata(metaHeader, src);

    Ipv4Address dst = rrepHeader.GetDst();
    NS_LOG_LOGIC("RREP destination " << dst << " RREP origin " << rrepHeader.GetOrigin());

    uint8_t hop = rrepHeader.GetHopCount() + 1;
    rrepHeader.SetHopCount(hop);

    // Update GTT for the destination
    m_gtt.AddOrUpdateEntry(dst, rrepHeader.GetDstSeqno(), hop);

    // Improvement 3: Passive learning — the sender (previous hop) is alive at 1 hop
    {
        GttEntry senderEntry;
        if (m_gtt.LookupEntry(src, senderEntry))
        {
            m_gtt.RefreshEntry(src, senderEntry.seqNo);
        }
        else
        {
            m_gtt.AddOrUpdateEntry(src, 0, 1);
        }
    }

    // Create or update route to destination
    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(my));
    RoutingTableEntry newEntry(
        /*dev=*/dev,
        /*dst=*/dst,
        /*vSeqNo=*/true,
        /*seqNo=*/rrepHeader.GetDstSeqno(),
        /*iface=*/m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(my), 0),
        /*hops=*/hop,
        /*nextHop=*/src,
        /*lifetime=*/rrepHeader.GetLifeTime());

    RoutingTableEntry toDst;
    if (m_routingTable.LookupRoute(dst, toDst))
    {
        if ((!toDst.GetValidSeqNo()) ||
            ((int32_t(rrepHeader.GetDstSeqno()) - int32_t(toDst.GetSeqNo())) > 0) ||
            (rrepHeader.GetDstSeqno() == toDst.GetSeqNo() && toDst.GetFlag() != VALID) ||
            (rrepHeader.GetDstSeqno() == toDst.GetSeqNo() && hop < toDst.GetHop()))
        {
            m_routingTable.Update(newEntry);
        }
    }
    else
    {
        NS_LOG_LOGIC("add new route");
        m_routingTable.AddRoute(newEntry);
    }

    // Acknowledge receipt of the RREP by sending RREP_ACK back
    if (rrepHeader.GetAckRequired())
    {
        SendReplyAck(src);
        rrepHeader.SetAckRequired(false);
    }

    NS_LOG_LOGIC("receiver " << my << " origin " << rrepHeader.GetOrigin());
    if (IsMyOwnAddress(rrepHeader.GetOrigin()))
    {
        if (toDst.GetFlag() == IN_SEARCH)
        {
            m_routingTable.Update(newEntry);
            m_addressReqTimer[dst].Cancel();
            m_addressReqTimer.erase(dst);
        }
        m_routingTable.LookupRoute(dst, toDst);

        // Fire route discovery trace with latency (not absolute time)
        {
            Time latency;
            auto startIt = m_rreqStartTime.find(dst);
            if (startIt != m_rreqStartTime.end())
            {
                latency = Simulator::Now() - startIt->second;
                m_rreqStartTime.erase(startIt);
            }
            else
            {
                latency = Seconds(0); // fallback: no recorded start time
            }
            m_routeDiscoveryTrace(dst, latency);
        }

        // Clear Smart TTL tracking — route found successfully
        m_smartTtlUsed.erase(dst);

        SendPacketFromQueue(dst, toDst.GetRoute());
        return;
    }

    // Forward RREP toward originator
    RoutingTableEntry toOrigin;
    if (!m_routingTable.LookupRoute(rrepHeader.GetOrigin(), toOrigin) ||
        toOrigin.GetFlag() == IN_SEARCH)
    {
        return; // drop
    }
    toOrigin.SetLifeTime(std::max(m_activeRouteTimeout, toOrigin.GetLifeTime()));
    m_routingTable.Update(toOrigin);

    // Update precursors
    if (m_routingTable.LookupValidRoute(rrepHeader.GetDst(), toDst))
    {
        toDst.InsertPrecursor(toOrigin.GetNextHop());
        m_routingTable.Update(toDst);

        RoutingTableEntry toNextHopToDst;
        m_routingTable.LookupRoute(toDst.GetNextHop(), toNextHopToDst);
        toNextHopToDst.InsertPrecursor(toOrigin.GetNextHop());
        m_routingTable.Update(toNextHopToDst);

        toOrigin.InsertPrecursor(toDst.GetNextHop());
        m_routingTable.Update(toOrigin);

        RoutingTableEntry toNextHopToOrigin;
        m_routingTable.LookupRoute(toOrigin.GetNextHop(), toNextHopToOrigin);
        toNextHopToOrigin.InsertPrecursor(toDst.GetNextHop());
        m_routingTable.Update(toNextHopToOrigin);
    }

    SocketIpTtlTag tag;
    p->RemovePacketTag(tag);
    if (tag.GetTtl() < 2)
    {
        NS_LOG_DEBUG("TTL exceeded. Drop RREP destination " << dst << " origin "
                                                            << rrepHeader.GetOrigin());
        return;
    }

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag ttl;
    ttl.SetTtl(tag.GetTtl() - 1);
    packet->AddPacketTag(ttl);

    // Piggyback metadata on forwarded RREP
    TopologyMetadataHeader forwardMeta = BuildTopologyMetadata();
    packet->AddHeader(forwardMeta);
    packet->AddHeader(rrepHeader);
    TypeHeader tHeader(TAVRN_E_RREP);
    packet->AddHeader(tHeader);

    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    NS_ASSERT(socket);
    // Account for forwarded RREP in control overhead trace
    m_controlOverheadTrace(packet->GetSize());
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), TAVRN_PORT));
}

// ============================================================================
// RecvError — Process E_RERR
// ============================================================================

void
RoutingProtocol::RecvError(Ptr<Packet> p, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << " from " << src);
    ERerrHeader rerrHeader;
    p->RemoveHeader(rerrHeader);

    // Extract piggybacked topology metadata from E_RERR
    TopologyMetadataHeader metaHeader;
    p->RemoveHeader(metaHeader);
    ProcessTopologyMetadata(metaHeader, src);

    // Improvement 3: Passive learning — the RERR sender is alive at 1 hop
    {
        GttEntry senderEntry;
        if (m_gtt.LookupEntry(src, senderEntry))
        {
            m_gtt.RefreshEntry(src, senderEntry.seqNo);
        }
        else
        {
            m_gtt.AddOrUpdateEntry(src, 0, 1);
        }
    }

    std::map<Ipv4Address, uint32_t> dstWithNextHopSrc;
    std::map<Ipv4Address, uint32_t> unreachable;
    m_routingTable.GetListOfDestinationWithNextHop(src, dstWithNextHopSrc);
    std::pair<Ipv4Address, uint32_t> un;
    while (rerrHeader.RemoveUnDestination(un))
    {
        for (auto i = dstWithNextHopSrc.begin(); i != dstWithNextHopSrc.end(); ++i)
        {
            if (i->first == un.first)
            {
                unreachable.insert(un);
            }
        }
    }

    std::vector<Ipv4Address> precursors;
    for (auto i = unreachable.begin(); i != unreachable.end();)
    {
        if (!rerrHeader.AddUnDestination(i->first, i->second))
        {
            TypeHeader typeHeader(TAVRN_E_RERR);
            Ptr<Packet> packet = Create<Packet>();
            SocketIpTtlTag tag;
            tag.SetTtl(1);
            packet->AddPacketTag(tag);
            // Piggyback topology metadata on forwarded split RERR
            TopologyMetadataHeader splitMeta = BuildTopologyMetadata();
            packet->AddHeader(splitMeta);
            packet->AddHeader(rerrHeader);
            packet->AddHeader(typeHeader);
            SendRerrMessage(packet, precursors);
            rerrHeader.Clear();
        }
        else
        {
            RoutingTableEntry toDst;
            m_routingTable.LookupRoute(i->first, toDst);
            toDst.GetPrecursors(precursors);
            ++i;
        }
    }
    if (rerrHeader.GetDestCount() != 0)
    {
        TypeHeader typeHeader(TAVRN_E_RERR);
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        // Piggyback topology metadata on forwarded RERR
        TopologyMetadataHeader fwdMeta = BuildTopologyMetadata();
        packet->AddHeader(fwdMeta);
        packet->AddHeader(rerrHeader);
        packet->AddHeader(typeHeader);
        SendRerrMessage(packet, precursors);
    }
    m_routingTable.InvalidateRoutesWithDst(unreachable);
}

// ============================================================================
// SendRequest — Initiate route discovery by broadcasting E_RREQ
// ============================================================================

void
RoutingProtocol::SendRequest(Ipv4Address dst)
{
    NS_LOG_FUNCTION(this << dst);
    // Rate limit check
    if (m_rreqCount == m_rreqRateLimit)
    {
        Simulator::Schedule(m_rreqRateLimitTimer.GetDelayLeft() + MicroSeconds(100),
                            &RoutingProtocol::SendRequest,
                            this,
                            dst);
        return;
    }
    else
    {
        m_rreqCount++;
    }

    // Create RREQ header
    ERreqHeader rreqHeader;
    rreqHeader.SetDst(dst);

    RoutingTableEntry rt;
    uint16_t ttl = m_ttlStart;
    if (m_routingTable.LookupRoute(dst, rt))
    {
        if (rt.GetFlag() != IN_SEARCH)
        {
            ttl = std::min<uint16_t>(rt.GetHop() + m_ttlIncrement, m_netDiameter);
        }
        else
        {
            // GTT-Assisted Route Discovery: If the GTT knows the destination's
            // hop count, skip the Expanding Ring Search and use a targeted TTL.
            // This eliminates wasted broadcast waves (TTL=1,3,5,7) when we
            // already know the destination is e.g. 8 hops away.
            //
            // If Smart TTL was already attempted for this destination
            // and failed (we're back in IN_SEARCH), skip GTT and go straight
            // to full flood (m_netDiameter). This prevents retry loops where
            // the same too-small TTL is reused indefinitely.
            //
            // Only trust GTT hop counts from entries with positive
            // TTL remaining (not hard-expired/stale).
            if (m_smartTtlUsed.count(dst) > 0)
            {
                // Smart TTL already failed once — full flood fallback
                ttl = m_netDiameter;
                m_smartTtlUsed.erase(dst);
                NS_LOG_DEBUG("Smart TTL retry fallback: dst=" << dst
                             << " forcing netDiameter=" << m_netDiameter);
            }
            else
            {
                GttEntry gttDst;
                if (m_gtt.LookupEntry(dst, gttDst) && gttDst.hopCount > 0 &&
                    !gttDst.departed && m_gtt.TtlRemaining(dst).GetSeconds() > 0)
                {
                    // Smart TTL: hop count + 2 buffer for topology changes
                    ttl = std::min<uint16_t>(gttDst.hopCount + 2, m_netDiameter);
                    m_smartTtlUsed.insert(dst);
                    NS_LOG_DEBUG("GTT-Assisted RREQ: dst=" << dst
                                 << " gttHops=" << gttDst.hopCount
                                 << " smartTtl=" << ttl);
                }
                else
                {
                    // No fresh GTT hop count info — fall back to standard ERS
                    ttl = rt.GetHop() + m_ttlIncrement;
                    if (ttl > m_ttlThreshold)
                    {
                        ttl = m_netDiameter;
                    }
                }
            }
        }
        if (ttl == m_netDiameter)
        {
            rt.IncrementRreqCnt();
        }
        if (rt.GetValidSeqNo())
        {
            rreqHeader.SetDstSeqno(rt.GetSeqNo());
        }
        else
        {
            rreqHeader.SetUnknownSeqno(true);
        }
        rt.SetHop(ttl);
        rt.SetFlag(IN_SEARCH);
        rt.SetLifeTime(m_pathDiscoveryTime);
        m_routingTable.Update(rt);
    }
    else
    {
        // First discovery attempt — no route exists yet.
        // Check GTT for a smart initial TTL before falling back to ERS.
        // Only trust non-expired GTT entries (TtlRemaining > 0).
        GttEntry gttDst;
        if (m_gtt.LookupEntry(dst, gttDst) && gttDst.hopCount > 0 &&
            !gttDst.departed && m_gtt.TtlRemaining(dst).GetSeconds() > 0)
        {
            ttl = std::min<uint16_t>(gttDst.hopCount + 2, m_netDiameter);
            m_smartTtlUsed.insert(dst);
            NS_LOG_DEBUG("GTT-Assisted first RREQ: dst=" << dst
                         << " gttHops=" << gttDst.hopCount
                         << " smartTtl=" << ttl);
        }
        // else ttl stays at m_ttlStart (standard ERS start)

        rreqHeader.SetUnknownSeqno(true);
        Ptr<NetDevice> dev = nullptr;
        RoutingTableEntry newEntry(/*dev=*/dev,
                                   /*dst=*/dst,
                                   /*vSeqNo=*/false,
                                   /*seqNo=*/0,
                                   /*iface=*/Ipv4InterfaceAddress(),
                                   /*hops=*/ttl,
                                   /*nextHop=*/Ipv4Address(),
                                   /*lifetime=*/m_pathDiscoveryTime);
        if (ttl == m_netDiameter)
        {
            newEntry.IncrementRreqCnt();
        }
        newEntry.SetFlag(IN_SEARCH);
        m_routingTable.AddRoute(newEntry);
    }

    if (m_gratuitousReply)
    {
        rreqHeader.SetGratuitousRrep(true);
    }
    if (m_destinationOnly)
    {
        rreqHeader.SetDestinationOnly(true);
    }

    m_seqNo++;
    rreqHeader.SetOriginSeqno(m_seqNo);
    m_requestId++;
    rreqHeader.SetId(m_requestId);

    // Record RREQ start time for route discovery latency measurement
    if (m_rreqStartTime.find(dst) == m_rreqStartTime.end())
    {
        m_rreqStartTime[dst] = Simulator::Now();
    }

    // Build topology metadata for piggybacking
    TopologyMetadataHeader metaHeader = BuildTopologyMetadata();

    // Send RREQ as subnet directed broadcast from each interface
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;

        rreqHeader.SetOrigin(iface.GetLocal());
        m_rreqIdCache.IsDuplicate(iface.GetLocal(), m_requestId);

        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(ttl);
        packet->AddPacketTag(tag);
        packet->AddHeader(metaHeader);
        packet->AddHeader(rreqHeader);
        TypeHeader tHeader(TAVRN_E_RREQ);
        packet->AddHeader(tHeader);

        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        NS_LOG_DEBUG("Send E_RREQ with id " << rreqHeader.GetId() << " to socket");
        m_lastBcastTime = Simulator::Now();
        m_controlOverheadTrace(packet->GetSize());
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            packet,
                            destination);
    }
    ScheduleRreqRetry(dst);
}

// ============================================================================
// SendReply — Send E_RREP in response to E_RREQ
// ============================================================================

void
RoutingProtocol::SendReply(const ERreqHeader& rreqHeader, const RoutingTableEntry& toOrigin)
{
    NS_LOG_FUNCTION(this << toOrigin.GetDestination());

    if (!rreqHeader.GetUnknownSeqno() && (rreqHeader.GetDstSeqno() == m_seqNo + 1))
    {
        m_seqNo++;
    }

    ERrepHeader rrepHeader(/*hopCount=*/0,
                           /*dst=*/rreqHeader.GetDst(),
                           /*dstSeqNo=*/m_seqNo,
                           /*origin=*/toOrigin.GetDestination(),
                           /*lifetime=*/m_myRouteTimeout);

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(toOrigin.GetHop());
    packet->AddPacketTag(tag);

    // Piggyback topology metadata
    TopologyMetadataHeader metaHeader = BuildTopologyMetadata();
    packet->AddHeader(metaHeader);
    packet->AddHeader(rrepHeader);
    TypeHeader tHeader(TAVRN_E_RREP);
    packet->AddHeader(tHeader);

    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    NS_ASSERT(socket);
    m_controlOverheadTrace(packet->GetSize());
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), TAVRN_PORT));
}

// ============================================================================
// SendReplyByIntermediateNode
// ============================================================================

void
RoutingProtocol::SendReplyByIntermediateNode(RoutingTableEntry& toDst,
                                             RoutingTableEntry& toOrigin,
                                             bool gratRep)
{
    NS_LOG_FUNCTION(this);
    ERrepHeader rrepHeader(/*hopCount=*/toDst.GetHop(),
                           /*dst=*/toDst.GetDestination(),
                           /*dstSeqNo=*/toDst.GetSeqNo(),
                           /*origin=*/toOrigin.GetDestination(),
                           /*lifetime=*/toDst.GetLifeTime());

    // If destination is a direct neighbor, the link back toward origin may be
    // unidirectional. Request RREP-ACK to verify (matches AODV RFC 3561 §6.8).
    if (toDst.GetHop() == 1)
    {
        rrepHeader.SetAckRequired(true);
        Ipv4Address nextHopAddr = toOrigin.GetNextHop();
        // Cancel any existing pending ACK timer for this neighbor
        auto it = m_pendingAckEvents.find(nextHopAddr);
        if (it != m_pendingAckEvents.end())
        {
            Simulator::Cancel(it->second);
        }
        EventId ackEvent = Simulator::Schedule(m_nextHopWait,
                                               &RoutingProtocol::AckTimerExpire,
                                               this,
                                               nextHopAddr,
                                               m_blackListTimeout);
        m_pendingAckEvents[nextHopAddr] = ackEvent;
    }
    toDst.InsertPrecursor(toOrigin.GetNextHop());
    toOrigin.InsertPrecursor(toDst.GetNextHop());
    m_routingTable.Update(toDst);
    m_routingTable.Update(toOrigin);

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(toOrigin.GetHop());
    packet->AddPacketTag(tag);

    TopologyMetadataHeader metaHeader = BuildTopologyMetadata();
    packet->AddHeader(metaHeader);
    packet->AddHeader(rrepHeader);
    TypeHeader tHeader(TAVRN_E_RREP);
    packet->AddHeader(tHeader);

    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    NS_ASSERT(socket);
    m_controlOverheadTrace(packet->GetSize());
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), TAVRN_PORT));

    // Generating gratuitous RREPs
    if (gratRep)
    {
        ERrepHeader gratRepHeader(/*hopCount=*/toOrigin.GetHop(),
                                  /*dst=*/toOrigin.GetDestination(),
                                  /*dstSeqNo=*/toOrigin.GetSeqNo(),
                                  /*origin=*/toDst.GetDestination(),
                                  /*lifetime=*/toOrigin.GetLifeTime());
        Ptr<Packet> packetToDst = Create<Packet>();
        SocketIpTtlTag gratTag;
        gratTag.SetTtl(toDst.GetHop());
        packetToDst->AddPacketTag(gratTag);

        TopologyMetadataHeader gratMeta = BuildTopologyMetadata();
        packetToDst->AddHeader(gratMeta);
        packetToDst->AddHeader(gratRepHeader);
        TypeHeader type(TAVRN_E_RREP);
        packetToDst->AddHeader(type);

        Ptr<Socket> socket = FindSocketWithInterfaceAddress(toDst.GetInterface());
        NS_ASSERT(socket);
        NS_LOG_LOGIC("Send gratuitous RREP " << packetToDst->GetUid());
        m_controlOverheadTrace(packetToDst->GetSize());
        socket->SendTo(packetToDst, 0, InetSocketAddress(toDst.GetNextHop(), TAVRN_PORT));
    }
}

// ============================================================================
// SendRerrWhenBreaksLinkToNextHop
// ============================================================================

void
RoutingProtocol::SendRerrWhenBreaksLinkToNextHop(Ipv4Address nextHop)
{
    NS_LOG_FUNCTION(this << nextHop);
    ERerrHeader rerrHeader;
    std::vector<Ipv4Address> precursors;
    std::map<Ipv4Address, uint32_t> unreachable;

    RoutingTableEntry toNextHop;
    if (!m_routingTable.LookupRoute(nextHop, toNextHop))
    {
        return;
    }
    toNextHop.GetPrecursors(precursors);
    rerrHeader.AddUnDestination(nextHop, toNextHop.GetSeqNo());
    m_routingTable.GetListOfDestinationWithNextHop(nextHop, unreachable);

    for (auto i = unreachable.begin(); i != unreachable.end();)
    {
        if (!rerrHeader.AddUnDestination(i->first, i->second))
        {
            NS_LOG_LOGIC("Send RERR message with maximum size.");
            TypeHeader typeHeader(TAVRN_E_RERR);
            Ptr<Packet> packet = Create<Packet>();
            SocketIpTtlTag tag;
            tag.SetTtl(1);
            packet->AddPacketTag(tag);
            // Piggyback topology metadata on split RERR
            TopologyMetadataHeader splitMeta = BuildTopologyMetadata();
            packet->AddHeader(splitMeta);
            packet->AddHeader(rerrHeader);
            packet->AddHeader(typeHeader);
            SendRerrMessage(packet, precursors);
            rerrHeader.Clear();
        }
        else
        {
            RoutingTableEntry toDst;
            m_routingTable.LookupRoute(i->first, toDst);
            toDst.GetPrecursors(precursors);
            ++i;
        }
    }
    if (rerrHeader.GetDestCount() != 0)
    {
        TypeHeader typeHeader(TAVRN_E_RERR);
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        // Piggyback topology metadata on RERR
        TopologyMetadataHeader meta = BuildTopologyMetadata();
        packet->AddHeader(meta);
        packet->AddHeader(rerrHeader);
        packet->AddHeader(typeHeader);
        SendRerrMessage(packet, precursors);
    }
    unreachable.insert(std::make_pair(nextHop, toNextHop.GetSeqNo()));
    m_routingTable.InvalidateRoutesWithDst(unreachable);
}

// ============================================================================
// SendRerrMessage
// ============================================================================

void
RoutingProtocol::SendRerrMessage(Ptr<Packet> packet, std::vector<Ipv4Address> precursors)
{
    NS_LOG_FUNCTION(this);

    if (precursors.empty())
    {
        NS_LOG_LOGIC("No precursors");
        return;
    }
    if (m_rerrCount == m_rerrRateLimit)
    {
        NS_ASSERT(m_rerrRateLimitTimer.IsRunning());
        NS_LOG_LOGIC("RerrRateLimit reached; suppressing RERR");
        return;
    }

    // If there is only one precursor, RERR SHOULD be unicast
    if (precursors.size() == 1)
    {
        RoutingTableEntry toPrecursor;
        if (m_routingTable.LookupValidRoute(precursors.front(), toPrecursor))
        {
            Ptr<Socket> socket = FindSocketWithInterfaceAddress(toPrecursor.GetInterface());
            NS_ASSERT(socket);
            NS_LOG_LOGIC("one precursor => unicast RERR to " << toPrecursor.GetDestination());
            m_controlOverheadTrace(packet->GetSize());
            Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                                &RoutingProtocol::SendTo,
                                this,
                                socket,
                                packet,
                                precursors.front());
            m_rerrCount++;
        }
        return;
    }

    // Broadcast RERR on interfaces that have precursor nodes
    std::vector<Ipv4InterfaceAddress> ifaces;
    RoutingTableEntry toPrecursor;
    for (auto i = precursors.begin(); i != precursors.end(); ++i)
    {
        if (m_routingTable.LookupValidRoute(*i, toPrecursor) &&
            std::find(ifaces.begin(), ifaces.end(), toPrecursor.GetInterface()) == ifaces.end())
        {
            ifaces.push_back(toPrecursor.GetInterface());
        }
    }

    // Increment RERR count for broadcast path too
    m_rerrCount++;

    for (auto i = ifaces.begin(); i != ifaces.end(); ++i)
    {
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(*i);
        NS_ASSERT(socket);
        NS_LOG_LOGIC("Broadcast RERR message from interface " << i->GetLocal());
        Ptr<Packet> p = packet->Copy();
        Ipv4Address destination;
        if (i->GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = i->GetBroadcast();
        }
        m_controlOverheadTrace(p->GetSize());
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            p,
                            destination);
    }
}

// ============================================================================
// SendRerrWhenNoRouteToForward
// ============================================================================

void
RoutingProtocol::SendRerrWhenNoRouteToForward(Ipv4Address dst,
                                               uint32_t dstSeqNo,
                                               Ipv4Address origin)
{
    NS_LOG_FUNCTION(this);
    if (m_rerrCount == m_rerrRateLimit)
    {
        NS_ASSERT(m_rerrRateLimitTimer.IsRunning());
        NS_LOG_LOGIC("RerrRateLimit reached; suppressing RERR");
        return;
    }
    // Increment RERR count after rate limit check passes
    m_rerrCount++;

    ERerrHeader rerrHeader;
    rerrHeader.AddUnDestination(dst, dstSeqNo);
    RoutingTableEntry toOrigin;
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    // Piggyback topology metadata on RERR
    TopologyMetadataHeader meta = BuildTopologyMetadata();
    packet->AddHeader(meta);
    packet->AddHeader(rerrHeader);
    packet->AddHeader(TypeHeader(TAVRN_E_RERR));

    m_controlOverheadTrace(packet->GetSize());

    if (m_routingTable.LookupValidRoute(origin, toOrigin))
    {
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
        NS_ASSERT(socket);
        NS_LOG_LOGIC("Unicast RERR to the source of the data transmission");
        socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), TAVRN_PORT));
    }
    else
    {
        for (auto i = m_socketAddresses.begin(); i != m_socketAddresses.end(); ++i)
        {
            Ptr<Socket> socket = i->first;
            Ipv4InterfaceAddress iface = i->second;
            NS_ASSERT(socket);
            NS_LOG_LOGIC("Broadcast RERR message from interface " << iface.GetLocal());
            Ipv4Address destination;
            if (iface.GetMask() == Ipv4Mask::GetOnes())
            {
                destination = Ipv4Address("255.255.255.255");
            }
            else
            {
                destination = iface.GetBroadcast();
            }
            socket->SendTo(packet->Copy(), 0, InetSocketAddress(destination, TAVRN_PORT));
        }
    }
}

// ============================================================================
// SendPacketFromQueue
// ============================================================================

void
RoutingProtocol::SendPacketFromQueue(Ipv4Address dst, Ptr<Ipv4Route> route)
{
    NS_LOG_FUNCTION(this);
    QueueEntry queueEntry;
    while (m_queue.Dequeue(dst, queueEntry))
    {
        DeferredRouteOutputTag tag;
        Ptr<Packet> p = ConstCast<Packet>(queueEntry.GetPacket());
        if (p->RemovePacketTag(tag) && tag.GetInterface() != -1 &&
            tag.GetInterface() != m_ipv4->GetInterfaceForDevice(route->GetOutputDevice()))
        {
            NS_LOG_DEBUG("Output device doesn't match. Dropped.");
            return;
        }
        UnicastForwardCallback ucb = queueEntry.GetUnicastForwardCallback();
        Ipv4Header header = queueEntry.GetIpv4Header();
        header.SetSource(route->GetSource());
        header.SetTtl(header.GetTtl() + 1); // compensate extra TTL decrement by fake loopback

        // Principle 3: Piggyback topology metadata on outgoing data packets.
        // Improvement 1: Only piggyback when soft-expired entries need propagation.
        // Store this node's address so next hop attributes metadata correctly.
        TopologyMetadataHeader meta = BuildConditionalMetadata();
        if (meta.GetEntryCount() > 0)
        {
            p->AddHeader(meta);
            TavrnShimTag shimTag;
            shimTag.SetPrevHop(route->GetSource());
            p->AddPacketTag(shimTag);
        }
        ucb(route, p, header);
    }
}

// ============================================================================
// SendTo
// ============================================================================

void
RoutingProtocol::SendTo(Ptr<Socket> socket, Ptr<Packet> packet, Ipv4Address destination)
{
    socket->SendTo(packet, 0, InetSocketAddress(destination, TAVRN_PORT));
}

// ============================================================================
// RecvHello — Process HELLO message
// ============================================================================

void
RoutingProtocol::RecvHello(Ptr<Packet> p, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << src);
    HelloHeader helloHeader;
    p->RemoveHeader(helloHeader);

    // Try to extract piggybacked TopologyMetadataHeader (freshness response)
    if (p->GetSize() > 0)
    {
        TopologyMetadataHeader metaHeader;
        p->RemoveHeader(metaHeader);
        if (metaHeader.GetEntryCount() > 0)
        {
            ProcessTopologyMetadata(metaHeader, src);
        }
    }

    Ipv4Address nodeAddr = helloHeader.GetNodeAddr();

    // Add/update sender in GTT
    m_gtt.AddOrUpdateEntry(nodeAddr, helloHeader.GetSeqNo(), 1);

    // Use allowedHelloLoss * helloInterval for neighbor expiry when periodic HELLO
    // is enabled. Otherwise fall back to activeRouteTimeout.
    Time neighborExpiry = m_enablePeriodicHello
        ? Time(m_allowedHelloLoss * m_helloInterval)
        : m_activeRouteTimeout;
    m_nb.Update(src, neighborExpiry);

    // Record real RSSI from PHY sniffer for 1-hop neighbor
    double rssi = LookupRssiForIp(src);
    m_nb.SetRssi(src, rssi);

    // If this is a new node and we are bootstrapped, trigger mentorship offer
    if (helloHeader.GetIsNew() && m_isBootstrapped)
    {
        // Skip if we've already offered to mentor this node recently
        if (m_recentlyMentored.find(src) != m_recentlyMentored.end())
        {
            NS_LOG_DEBUG("Already offered to mentor " << src << ", skipping");
            return;
        }

        // Compute RSSI-proportional backoff: stronger signal -> shorter wait
        double rssi = m_nb.GetRssi(src);
        double backoffMs;
        if (rssi < -90.0 || rssi == 0.0)
        {
            backoffMs = 500.0; // weak signal or unknown -> long wait
        }
        else if (rssi > -30.0)
        {
            backoffMs = 10.0; // very strong signal -> short wait
        }
        else
        {
            // Linear mapping: -90 dBm -> 500ms, -30 dBm -> 10ms
            backoffMs = 500.0 - ((rssi + 90.0) / 60.0) * 490.0;
        }
        // Add some jitter
        backoffMs += m_uniformRandomVariable->GetValue(0.0, 50.0);
        NS_LOG_DEBUG("Scheduling SyncOffer to " << src << " with backoff "
                                                 << backoffMs << " ms");
        // Track the scheduled event so it can be cancelled if we overhear another offer
        EventId offerEvent = Simulator::Schedule(
            MilliSeconds(static_cast<uint64_t>(backoffMs)),
            &RoutingProtocol::SendSyncOffer,
            this,
            src);
        m_pendingSyncOfferEvents[src] = offerEvent;
    }
}

// ============================================================================
// SendHello — Broadcast HELLO message
// ============================================================================

void
RoutingProtocol::SendHello()
{
    NS_LOG_FUNCTION(this);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;

        HelloHeader helloHeader(iface.GetLocal(), m_seqNo, !m_isBootstrapped);

        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        packet->AddHeader(helloHeader);
        TypeHeader tHeader(TAVRN_HELLO);
        packet->AddHeader(tHeader);

        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        m_controlOverheadTrace(packet->GetSize());
        Time jitter = MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10));
        Simulator::Schedule(jitter, &RoutingProtocol::SendTo, this, socket, packet, destination);
    }
    m_lastBcastTime = Simulator::Now();
}

// ============================================================================
// RecvSyncOffer — Process SYNC_OFFER from a potential mentor
// ============================================================================

void
RoutingProtocol::RecvSyncOffer(Ptr<Packet> p, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << src);
    SyncOfferHeader offerHeader;
    p->RemoveHeader(offerHeader);

    // Broadcast dampening — if we overhear a SYNC_OFFER for a mentee we have a
    // pending offer for, cancel our pending offer (another node won the race).
    Ipv4Address offeredMentee = offerHeader.GetMenteeAddr();
    if (offeredMentee != Ipv4Address())
    {
        auto pendingIt = m_pendingSyncOfferEvents.find(offeredMentee);
        if (pendingIt != m_pendingSyncOfferEvents.end())
        {
            NS_LOG_DEBUG("Overheard SYNC_OFFER for " << offeredMentee
                         << " from " << src << "; cancelling our pending offer");
            Simulator::Cancel(pendingIt->second);
            m_pendingSyncOfferEvents.erase(pendingIt);
        }
    }

    // Reject offers not addressed to us (broadcast SYNC_OFFER
    // may target a different mentee; we only did dampening above).
    {
        Ipv4Address myAddr;
        if (!m_socketAddresses.empty())
        {
            myAddr = m_socketAddresses.begin()->second.GetLocal();
        }
        if (offeredMentee != Ipv4Address() && offeredMentee != myAddr)
        {
            NS_LOG_DEBUG("SYNC_OFFER from " << src << " is for mentee " << offeredMentee
                         << ", not us (" << myAddr << "). Ignoring.");
            return;
        }
    }

    // If already bootstrapped or already have a mentor, ignore
    if (m_isBootstrapped || m_mentor != Ipv4Address())
    {
        NS_LOG_DEBUG("Ignoring SYNC_OFFER from " << src
                     << " (already bootstrapped or have mentor)");
        return;
    }

    // Cancel bootstrap self-promotion timer since we received an offer
    m_bootstrapTimer.Cancel();

    // Collect offers during a short window instead of accepting first blindly
    // Use src (IP-layer sender) as mentor identity, not header field (anti-spoof)
    SyncOfferCandidate candidate;
    candidate.mentorAddr = src;
    candidate.gttSize = offerHeader.GetGttSize();
    m_syncOfferCandidates.push_back(candidate);

    NS_LOG_DEBUG("Collected SYNC_OFFER from " << candidate.mentorAddr
                 << " with GTT size " << candidate.gttSize
                 << " (total candidates: " << m_syncOfferCandidates.size() << ")");

    // Start collection timer on first offer
    if (m_syncOfferCandidates.size() == 1)
    {
        m_offerCollectionTimer.SetFunction(&RoutingProtocol::OfferCollectionExpire, this);
        m_offerCollectionTimer.Schedule(Seconds(2));
    }
}

// ============================================================================
// SendSyncOffer — Unicast SYNC_OFFER to a newly discovered node
// ============================================================================

void
RoutingProtocol::SendSyncOffer(Ipv4Address mentee)
{
    NS_LOG_FUNCTION(this << mentee);

    // Check if we already offered to this mentee recently
    if (m_recentlyMentored.find(mentee) != m_recentlyMentored.end())
    {
        NS_LOG_DEBUG("Already offered to mentor " << mentee << ", suppressing duplicate offer");
        return;
    }

    // Get the first interface address as our local address
    if (m_socketAddresses.empty())
    {
        return;
    }

    // Mark mentee as recently offered with tracked EventId (cancellable in DoDispose)
    // The EventId is stored in the map so DoDispose can cancel all pending cleanup events.
    // Uses CancelPendingSyncOffer as the cleanup callback (it erases from m_recentlyMentored).
    EventId cleanupEvent = Simulator::Schedule(Seconds(10),
                                               &RoutingProtocol::CancelPendingSyncOffer,
                                               this,
                                               mentee);
    m_recentlyMentored[mentee] = cleanupEvent;

    // Remove pending event tracking (we're sending now)
    m_pendingSyncOfferEvents.erase(mentee);

    // SYNC_OFFER must be BROADCAST so neighbors can overhear and cancel their pending offers
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;

        // Include mentee address in header for broadcast dampening
        SyncOfferHeader offerHeader(iface.GetLocal(), m_gtt.GetTotalEntries(), mentee);

        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        packet->AddHeader(offerHeader);
        TypeHeader tHeader(TAVRN_SYNC_OFFER);
        packet->AddHeader(tHeader);

        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        m_controlOverheadTrace(packet->GetSize());
        socket->SendTo(packet, 0, InetSocketAddress(destination, TAVRN_PORT));
    }
}

// ============================================================================
// RecvSyncPull — Process SYNC_PULL request from a mentee
// ============================================================================

void
RoutingProtocol::RecvSyncPull(Ptr<Packet> p, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << src);
    SyncPullHeader pullHeader;
    p->RemoveHeader(pullHeader);

    uint32_t startIndex = pullHeader.GetIndex();
    uint32_t count = pullHeader.GetCount();

    NS_LOG_DEBUG("RecvSyncPull from " << src << " index=" << startIndex << " count=" << count);

    SendSyncData(src, startIndex, count);
}

// ============================================================================
// SendSyncPull — Request next page of GTT entries from mentor
// ============================================================================

void
RoutingProtocol::SendSyncPull(Ipv4Address mentor)
{
    NS_LOG_FUNCTION(this << mentor);

    if (m_socketAddresses.empty())
    {
        return;
    }

    auto j = m_socketAddresses.begin();
    Ptr<Socket> socket = j->first;

    SyncPullHeader pullHeader(m_syncNextIndex, m_syncPageSize);

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    packet->AddHeader(pullHeader);
    TypeHeader tHeader(TAVRN_SYNC_PULL);
    packet->AddHeader(tHeader);

    m_controlOverheadTrace(packet->GetSize());
    socket->SendTo(packet, 0, InetSocketAddress(mentor, TAVRN_PORT));

    // Start SYNC_PULL timeout to detect mentor death
    m_syncPullTimeout.Cancel();
    m_syncPullTimeout.SetFunction(&RoutingProtocol::SyncPullTimeoutExpire, this);
    m_syncPullTimeout.Schedule(2 * m_netTraversalTime);
}

// ============================================================================
// SendSyncData — Send a page of GTT entries to a mentee
// ============================================================================

void
RoutingProtocol::SendSyncData(Ipv4Address mentee, uint32_t startIndex, uint32_t count)
{
    NS_LOG_FUNCTION(this << mentee << startIndex << count);

    if (m_socketAddresses.empty())
    {
        return;
    }

    auto j = m_socketAddresses.begin();
    Ptr<Socket> socket = j->first;

    std::vector<GttEntry> page = m_gtt.GetEntriesPage(startIndex, count);

    SyncDataHeader dataHeader;
    dataHeader.SetStartIndex(startIndex);
    dataHeader.SetTotalEntries(m_gtt.GetTotalEntries());

    for (const auto& entry : page)
    {
        SyncDataEntry sde;
        sde.nodeAddr = entry.nodeAddr;
        sde.lastSeen = static_cast<uint32_t>(entry.lastSeen.GetSeconds());
        Time remaining = entry.ttlExpiry - Simulator::Now();
        sde.ttlRemaining =
            static_cast<uint16_t>(std::max(0.0, remaining.GetSeconds()));
        sde.seqNo = entry.seqNo;           // carry mentor's seqNo
        sde.hopCount = entry.hopCount + 1;  // carry hop count (+1 for mentor hop)
        dataHeader.AddEntry(sde);
    }

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    packet->AddHeader(dataHeader);
    TypeHeader tHeader(TAVRN_SYNC_DATA);
    packet->AddHeader(tHeader);

    m_controlOverheadTrace(packet->GetSize());
    socket->SendTo(packet, 0, InetSocketAddress(mentee, TAVRN_PORT));
}

// ============================================================================
// RecvSyncData — Process received SYNC_DATA page from mentor
// ============================================================================

void
RoutingProtocol::RecvSyncData(Ptr<Packet> p, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << src);
    SyncDataHeader dataHeader;
    p->RemoveHeader(dataHeader);

    // Cancel SYNC_PULL timeout — we got a response
    m_syncPullTimeout.Cancel();
    m_syncPullRetries = 0;

    uint32_t totalEntries = dataHeader.GetTotalEntries();
    uint32_t startIndex = dataHeader.GetStartIndex();
    const auto& entries = dataHeader.GetEntries();

    NS_LOG_DEBUG("RecvSyncData from " << src << " startIndex=" << startIndex
                 << " count=" << entries.size() << " total=" << totalEntries);

    // Merge each entry into local GTT
    for (const auto& sde : entries)
    {
        Time now = Simulator::Now();
        Time ttlRemaining = Seconds(sde.ttlRemaining);
        Time lastSeen = Seconds(sde.lastSeen);

        GttEntry entry;
        entry.nodeAddr = sde.nodeAddr;
        entry.lastSeen = lastSeen;
        entry.ttlExpiry = now + ttlRemaining;
        entry.softExpiry = now + ttlRemaining * m_softExpiryThreshold;
        entry.seqNo = sde.seqNo;           // use mentor's seqNo
        entry.hopCount = sde.hopCount;      // use mentor's hopCount
        entry.departed = false;

        m_gtt.MergeEntry(entry);
    }

    // Check if more pages remain
    m_syncNextIndex = startIndex + static_cast<uint32_t>(entries.size());
    if (m_syncNextIndex < totalEntries)
    {
        // Schedule next pull
        m_syncTimer.SetFunction(&RoutingProtocol::SyncTimerExpire, this);
        m_syncTimer.Schedule(MilliSeconds(100));
    }
    else
    {
        // Sync complete — mark as bootstrapped
        m_isBootstrapped = true;
        m_mentor = Ipv4Address(); // Clear mentor

        // Cancel bootstrap timer since we completed sync
        m_bootstrapTimer.Cancel();

        NS_LOG_DEBUG("Mentorship sync complete. GTT has " << m_gtt.GetTotalEntries()
                     << " entries. Broadcasting TC-UPDATE(JOIN).");

        // Self is already in GTT from NotifyInterfaceUp, just refresh + announce
        if (!m_socketAddresses.empty())
        {
            Ipv4Address myAddr = m_socketAddresses.begin()->second.GetLocal();
            m_gtt.RefreshEntry(myAddr, m_seqNo);
            SendTcUpdate(myAddr, TcUpdateHeader::NODE_JOIN);
        }
    }
}

// ============================================================================
// SendTcUpdate — Originate a TC-UPDATE broadcast
// ============================================================================

void
RoutingProtocol::SendTcUpdate(Ipv4Address subjectAddr, TcUpdateHeader::EventType event)
{
    NS_LOG_FUNCTION(this << subjectAddr << static_cast<int>(event));

    if (m_socketAddresses.empty())
    {
        return;
    }

    Ipv4Address myAddr = m_socketAddresses.begin()->second.GetLocal();

    TcUpdateHeader tcHeader(myAddr,
                            m_tcUpdateSeqNo++,
                            subjectAddr,
                            event,
                            static_cast<uint32_t>(Simulator::Now().GetSeconds()));

    // Use separate TC UUID cache with proper lifetime (not RREQ cache)
    m_tcUuidCache.InsertUuid(tcHeader.GetUuid());

    // Broadcast on all interfaces
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;

        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(m_netDiameter);
        packet->AddPacketTag(tag);
        packet->AddHeader(tcHeader);
        TypeHeader tHeader(TAVRN_TC_UPDATE);
        packet->AddHeader(tHeader);

        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        m_controlOverheadTrace(packet->GetSize());
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            packet,
                            destination);
    }
    m_lastBcastTime = Simulator::Now();
}

// ============================================================================
// RecvTcUpdate — Process a received TC-UPDATE broadcast
// ============================================================================

void
RoutingProtocol::RecvTcUpdate(Ptr<Packet> p, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << src);
    TcUpdateHeader tcHeader;
    p->RemoveHeader(tcHeader);

    uint64_t uuid = tcHeader.GetUuid();

    // Use separate TC UUID cache for deduplication (proper 30s lifetime)
    if (m_tcUuidCache.IsUuidSeen(uuid))
    {
        NS_LOG_DEBUG("Ignoring duplicate TC-UPDATE UUID " << uuid);
        return;
    }

    // Mark UUID as seen
    m_tcUuidCache.InsertUuid(uuid);

    // Forwarding serves as implicit acknowledgment per gossip protocol convention.

    Ipv4Address subject = tcHeader.GetSubjectAddr();
    TcUpdateHeader::EventType event = tcHeader.GetEventType();

    NS_LOG_DEBUG("RecvTcUpdate: subject=" << subject << " event="
                 << (event == TcUpdateHeader::NODE_JOIN ? "JOIN" : "LEAVE"));

    // Improvement 2: Subject-based dedup — squash redundant floods for the same
    // death event (e.g. 4 neighbors all detect the same node failure via MAC).
    auto subjectKey = std::make_pair(subject, static_cast<uint8_t>(event));
    auto subjectIt = m_tcSubjectCache.find(subjectKey);
    if (subjectIt != m_tcSubjectCache.end() &&
        (Simulator::Now() - subjectIt->second) < Seconds(1))
    {
        NS_LOG_DEBUG("RecvTcUpdate: suppressing duplicate "
                     << (event == TcUpdateHeader::NODE_JOIN ? "JOIN" : "LEAVE")
                     << " for subject " << subject << " (within 1s window)");
        return;
    }
    m_tcSubjectCache[subjectKey] = Simulator::Now();

    // Lazy cleanup: periodically remove stale entries (> 2s old) to prevent unbounded growth.
    if (m_tcSubjectCache.size() > 50)
    {
        for (auto it = m_tcSubjectCache.begin(); it != m_tcSubjectCache.end(); )
        {
            if ((Simulator::Now() - it->second) > Seconds(2))
            {
                it = m_tcSubjectCache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Improvement 3: Passive learning — the TC-UPDATE sender is alive at 1 hop
    {
        GttEntry senderEntry;
        if (m_gtt.LookupEntry(src, senderEntry))
        {
            m_gtt.RefreshEntry(src, senderEntry.seqNo);
        }
        else
        {
            m_gtt.AddOrUpdateEntry(src, 0, 1);
        }
    }

    // Update local GTT
    if (event == TcUpdateHeader::NODE_JOIN)
    {
        // Only add if not already present; preserve existing seqNo/hopCount
        GttEntry existing;
        if (!m_gtt.LookupEntry(subject, existing))
        {
            m_gtt.AddOrUpdateEntry(subject, 0, 0);
        }
        else
        {
            m_gtt.RefreshEntry(subject, existing.seqNo);
        }
    }
    else // NODE_LEAVE
    {
        m_gtt.MarkDeparted(subject);
    }

    // Fire convergence trace
    Time eventTime = Seconds(tcHeader.GetTimestamp());
    m_convergenceTrace(subject, Simulator::Now() - eventTime);

    // Forward TC-UPDATE with decremented TTL (not reset to netDiameter)
    SocketIpTtlTag incomingTtl;
    uint8_t ttlValue = m_netDiameter; // fallback
    if (p->PeekPacketTag(incomingTtl))
    {
        ttlValue = incomingTtl.GetTtl();
    }
    if (ttlValue <= 1)
    {
        NS_LOG_DEBUG("TC-UPDATE TTL expired. Not forwarding.");
    }
    else
    {
        Ptr<Packet> forwardPacket = Create<Packet>();
        forwardPacket->AddHeader(tcHeader);
        ForwardTcUpdate(forwardPacket, ttlValue - 1);
    }
}

// ============================================================================
// ForwardTcUpdate — Re-broadcast a TC-UPDATE
// ============================================================================

void
RoutingProtocol::ForwardTcUpdate(Ptr<Packet> p, uint8_t ttl)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(ttl));

    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;

        Ptr<Packet> packet = p->Copy();
        TypeHeader tHeader(TAVRN_TC_UPDATE);
        packet->AddHeader(tHeader);

        SocketIpTtlTag tag;
        tag.SetTtl(ttl);  // use decremented TTL
        packet->AddPacketTag(tag);

        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        m_controlOverheadTrace(packet->GetSize());
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            packet,
                            destination);
    }
    m_lastBcastTime = Simulator::Now();
}

// ============================================================================
// BuildTopologyMetadata — Piggybacking
// ============================================================================

TopologyMetadataHeader
RoutingProtocol::BuildTopologyMetadata()
{
    NS_LOG_FUNCTION(this);
    TopologyMetadataHeader meta;
    uint8_t count = 0;

    // First priority: soft-expired entries (freshness requests)
    // Use round-robin cursor so we don't always pick the same
    // low-order addresses while others starve and hard-expire.
    std::vector<GttEntry> softExpired = m_gtt.GetSoftExpiredEntries();
    if (!softExpired.empty())
    {
        uint32_t startPos = m_metadataCursor % softExpired.size();
        for (uint32_t i = 0; i < softExpired.size() && count < m_maxMetadataEntries; ++i)
        {
            uint32_t idx = (startPos + i) % softExpired.size();
            const auto& entry = softExpired[idx];
            GttMetadataEntry mde;
            mde.nodeAddr = entry.nodeAddr;
            Time remaining = entry.ttlExpiry - Simulator::Now();
            mde.ttlRemaining = static_cast<uint16_t>(std::max(0.0, remaining.GetSeconds()));
            mde.flags = GttMetadataEntry::FLAG_FRESHNESS_REQUEST;
            meta.AddEntry(mde);
            count++;
        }
    }

    // Fill remaining slots with round-robin sampling (not always from start)
    if (count < m_maxMetadataEntries)
    {
        std::vector<Ipv4Address> allNodes = m_gtt.EnumerateNodes();
        if (!allNodes.empty())
        {
            uint32_t startPos = m_metadataCursor % allNodes.size();
            for (uint32_t i = 0; i < allNodes.size() && count < m_maxMetadataEntries; ++i)
            {
                uint32_t idx = (startPos + i) % allNodes.size();
                const auto& addr = allNodes[idx];

                // Skip entries already included as freshness requests
                bool alreadyIncluded = false;
                for (const auto& existing : meta.GetEntries())
                {
                    if (existing.nodeAddr == addr)
                    {
                        alreadyIncluded = true;
                        break;
                    }
                }
                if (alreadyIncluded)
                {
                    continue;
                }

                GttEntry entry;
                if (m_gtt.LookupEntry(addr, entry))
                {
                    GttMetadataEntry mde;
                    mde.nodeAddr = entry.nodeAddr;
                    Time remaining = entry.ttlExpiry - Simulator::Now();
                    mde.ttlRemaining =
                        static_cast<uint16_t>(std::max(0.0, remaining.GetSeconds()));
                    mde.flags = 0;
                    meta.AddEntry(mde);
                    count++;
                }
            }
            m_metadataCursor += m_maxMetadataEntries; // Advance cursor for next call
        }
    }

    return meta;
}

// ============================================================================
// BuildConditionalMetadata — Improvement 1: Conditional piggybacking for data packets
// ============================================================================

TopologyMetadataHeader
RoutingProtocol::BuildConditionalMetadata()
{
    NS_LOG_FUNCTION(this);
    TopologyMetadataHeader meta;

    // Only piggyback on data packets when there are soft-expired entries needing
    // freshness requests. If topology is stable, return an empty header (0 entries)
    // so data packets are transmitted clean (AODV-sized).
    std::vector<GttEntry> softExpired = m_gtt.GetSoftExpiredEntries();

    if (softExpired.empty())
    {
        // Mode A: No GTT expiries pending → clean packet (no metadata)
        NS_LOG_DEBUG("BuildConditionalMetadata: topology stable, returning empty metadata");
        return meta;
    }

    // Mode B: GTT expiries pending → piggyback up to MaxPiggybackEntries
    // Use round-robin cursor so entries with higher IP addresses
    // get fair access to piggyback slots, not always starting from the beginning.
    NS_LOG_DEBUG("BuildConditionalMetadata: " << softExpired.size()
                 << " soft-expired entries, piggybacking");
    uint8_t count = 0;
    Time now = Simulator::Now();
    uint32_t startPos = softExpired.empty() ? 0 : (m_conditionalCursor % softExpired.size());

    for (uint32_t i = 0; i < softExpired.size() && count < m_maxMetadataEntries; ++i)
    {
        uint32_t idx = (startPos + i) % softExpired.size();
        const auto& entry = softExpired[idx];

        // Check cooldown: skip entries that were recently piggybacked
        auto cooldownIt = m_piggybackCooldownMap.find(entry.nodeAddr);
        if (cooldownIt != m_piggybackCooldownMap.end() &&
            (now - cooldownIt->second) < m_piggybackCooldown)
        {
            continue; // Still in cooldown — skip this entry
        }

        GttMetadataEntry mde;
        mde.nodeAddr = entry.nodeAddr;
        Time remaining = entry.ttlExpiry - now;
        mde.ttlRemaining = static_cast<uint16_t>(std::max(0.0, remaining.GetSeconds()));
        mde.flags = GttMetadataEntry::FLAG_FRESHNESS_REQUEST;
        meta.AddEntry(mde);

        // Mark as piggybacked with cooldown
        m_piggybackCooldownMap[entry.nodeAddr] = now;
        count++;
    }
    m_conditionalCursor += m_maxMetadataEntries;  // Advance cursor for next call

    // Lazy cleanup of stale cooldown entries (> 2x cooldown period)
    if (m_piggybackCooldownMap.size() > 100)
    {
        Time cleanupThreshold = m_piggybackCooldown + m_piggybackCooldown;
        for (auto it = m_piggybackCooldownMap.begin(); it != m_piggybackCooldownMap.end(); )
        {
            if ((now - it->second) > cleanupThreshold)
            {
                it = m_piggybackCooldownMap.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    return meta;
}

// ============================================================================
// ProcessTopologyMetadata — Process piggybacked metadata
// ============================================================================

void
RoutingProtocol::ProcessTopologyMetadata(const TopologyMetadataHeader& meta, Ipv4Address sender)
{
    NS_LOG_FUNCTION(this << sender);

    // Determine our own address for tiered freshness response
    Ipv4Address myAddr;
    if (!m_socketAddresses.empty())
    {
        myAddr = m_socketAddresses.begin()->second.GetLocal();
    }

    const auto& entries = meta.GetEntries();
    for (const auto& mde : entries)
    {
        // Tiered freshness response per spec
        if (mde.flags & GttMetadataEntry::FLAG_FRESHNESS_REQUEST)
        {
            Time theirRemaining = Seconds(mde.ttlRemaining);
            Time ourRemaining = m_gtt.TtlRemaining(mde.nodeAddr);

            if (mde.nodeAddr == myAddr)
            {
                // Tier 1: I AM the target node — respond immediately
                Simulator::ScheduleNow(&RoutingProtocol::SendFreshnessResponse,
                                       this,
                                       sender,
                                       mde.nodeAddr);
            }
            else if (ourRemaining > theirRemaining * 2 && ourRemaining > Seconds(0))
            {
                // Tier 2: We have significantly fresher info — respond after short backoff
                Simulator::Schedule(
                    MilliSeconds(m_uniformRandomVariable->GetInteger(10, 100)),
                    &RoutingProtocol::SendFreshnessResponse,
                    this,
                    sender,
                    mde.nodeAddr);
            }
            // Tier 3: Marginal — suppress (do nothing)
        }

        // Always update GTT if the received info might be fresher
        GttEntry existing;
        if (!m_gtt.LookupEntry(mde.nodeAddr, existing))
        {
            // New node — add it
            m_gtt.AddOrUpdateEntry(mde.nodeAddr, 0, 0);
        }
        else
        {
            // Refresh if their TTL is longer than ours (implies fresher info)
            Time theirRemaining = Seconds(mde.ttlRemaining);
            Time ourRemaining = m_gtt.TtlRemaining(mde.nodeAddr);
            if (theirRemaining > ourRemaining)
            {
                m_gtt.RefreshEntry(mde.nodeAddr, existing.seqNo);
            }
        }
    }
}

// ============================================================================
// SendFreshnessResponse — Respond to a freshness request
// ============================================================================

void
RoutingProtocol::SendFreshnessResponse(Ipv4Address requester, Ipv4Address subject)
{
    NS_LOG_FUNCTION(this << requester << subject);

    // Send the SUBJECT's GTT data, not a generic HELLO about self
    GttEntry entry;
    if (!m_gtt.LookupEntry(subject, entry) || entry.departed)
    {
        return;
    }
    if (m_socketAddresses.empty())
    {
        return;
    }

    auto j = m_socketAddresses.begin();
    Ptr<Socket> socket = j->first;
    Ipv4InterfaceAddress iface = j->second;

    // Build a TopologyMetadataHeader with the subject's fresh GTT data
    TopologyMetadataHeader meta;
    GttMetadataEntry mde;
    mde.nodeAddr = entry.nodeAddr;
    Time remaining = entry.ttlExpiry - Simulator::Now();
    mde.ttlRemaining = static_cast<uint16_t>(std::max(0.0, remaining.GetSeconds()));
    mde.flags = 0; // This is a response, not a request
    meta.AddEntry(mde);

    // Send as a HELLO with piggybacked metadata about the subject
    HelloHeader helloHeader(iface.GetLocal(), m_seqNo, false);

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    packet->AddHeader(meta);        // Piggyback subject's fresh data
    packet->AddHeader(helloHeader);
    TypeHeader tHeader(TAVRN_HELLO);
    packet->AddHeader(tHeader);

    m_controlOverheadTrace(packet->GetSize());
    socket->SendTo(packet, 0, InetSocketAddress(requester, TAVRN_PORT));
}

// ============================================================================
// CheckGttExpiry — GTT maintenance
// ============================================================================

void
RoutingProtocol::CheckGttExpiry()
{
    NS_LOG_FUNCTION(this);

    // Determine our own address to skip self in expiry checks
    Ipv4Address myAddr;
    if (!m_socketAddresses.empty())
    {
        myAddr = m_socketAddresses.begin()->second.GetLocal();
    }

    // Get hard-expired entries and send verification E_RREQ for each
    std::vector<GttEntry> hardExpired = m_gtt.GetHardExpiredEntries();
    for (const auto& entry : hardExpired)
    {
        // Never expire or verify our own entry
        if (entry.nodeAddr == myAddr)
        {
            continue;
        }

        // Route-check bypass: if ActiveRouteTimeout > GttTtl, a valid route
        // proves the node is still reachable. Refresh GTT silently instead of
        // flooding a verification E_RREQ. Only flood when the route itself is
        // gone — natural graceful degradation.
        RoutingTableEntry rtEntry;
        if (m_routingTable.LookupValidRoute(entry.nodeAddr, rtEntry))
        {
            NS_LOG_DEBUG("GTT hard-expired for " << entry.nodeAddr
                         << " but valid route exists (via " << rtEntry.GetNextHop()
                         << "). Refreshing GTT silently.");
            m_gtt.AddOrUpdateEntry(entry.nodeAddr, entry.seqNo, rtEntry.GetHop());
            continue;
        }

        NS_LOG_DEBUG("Hard-expired GTT entry for " << entry.nodeAddr
                     << ". No valid route — checking verification state.");

        // Per-entry verification with probe-based retries (not tick-based)
        auto vit = m_verificationState.find(entry.nodeAddr);
        if (vit == m_verificationState.end())
        {
            // First time seeing this entry hard-expired — only start verification
            // if we can actually send the probe (rate limit allows it)
            if (m_rreqCount < m_rreqRateLimit)
            {
                VerificationInfo vi;
                vi.firstVerifyTime = Simulator::Now();
                vi.lastProbeTime = Simulator::Now();
                vi.retryCount = 1; // this probe counts as first attempt
                m_verificationState[entry.nodeAddr] = vi;

                NS_LOG_DEBUG("Starting verification for " << entry.nodeAddr);
                SendRequest(entry.nodeAddr);
            }
            // else: rate-limited, skip this cycle — do NOT create state without sending
        }
        else
        {
            // Already tracking verification — check if last probe timed out
            Time sinceLast = Simulator::Now() - vit->second.lastProbeTime;

            if (sinceLast < 2 * m_netTraversalTime)
            {
                // Still waiting for response from the last probe — do nothing
                continue;
            }

            // Last probe timed out
            if (vit->second.retryCount >= 2)
            {
                // No response after bounded retries — mark departed
                NS_LOG_DEBUG("Verification failed for " << entry.nodeAddr
                             << " after " << vit->second.retryCount
                             << " probes. Marking departed.");
                // Only send TC-UPDATE if state actually changed
                if (m_gtt.MarkDeparted(entry.nodeAddr))
                {
                    SendTcUpdate(entry.nodeAddr, TcUpdateHeader::NODE_LEAVE);
                }
                m_verificationState.erase(vit);
            }
            else if (m_rreqCount < m_rreqRateLimit)
            {
                // Retry verification — only increment if we actually send
                vit->second.retryCount++;
                vit->second.lastProbeTime = Simulator::Now();
                NS_LOG_DEBUG("Retry verification for " << entry.nodeAddr
                             << " (attempt " << vit->second.retryCount << ")");
                SendRequest(entry.nodeAddr);
            }
            // else: rate-limited, skip — counter not incremented
        }
    }

    // Clean up verification state for entries that are no longer hard-expired
    // (they got refreshed before we gave up)
    for (auto vit = m_verificationState.begin(); vit != m_verificationState.end();)
    {
        GttEntry e;
        if (!m_gtt.LookupEntry(vit->first, e) || e.departed ||
            Simulator::Now() < e.ttlExpiry)
        {
            vit = m_verificationState.erase(vit);
        }
        else
        {
            ++vit;
        }
    }

    // Purge old departed entries
    m_gtt.Purge();
}

// ============================================================================
// Timer expire handlers
// ============================================================================

void
RoutingProtocol::HelloTimerExpire()
{
    NS_LOG_FUNCTION(this);
    Time offset;
    if (m_lastBcastTime.IsStrictlyPositive())
    {
        offset = Simulator::Now() - m_lastBcastTime;
        NS_LOG_DEBUG("Hello deferred due to last bcast at:" << m_lastBcastTime);
    }
    else
    {
        SendHello();
    }
    m_htimer.Cancel();
    Time diff = m_helloInterval - offset;
    m_htimer.Schedule(std::max(Seconds(0), diff));
    m_lastBcastTime = Seconds(0);
}

void
RoutingProtocol::GttMaintenanceTimerExpire()
{
    NS_LOG_FUNCTION(this);

    // Refresh self entry in GTT before checking expiry
    // so our own entry never soft-expires or hard-expires
    if (!m_socketAddresses.empty())
    {
        Ipv4Address myAddr = m_socketAddresses.begin()->second.GetLocal();
        m_gtt.RefreshEntry(myAddr, m_seqNo);
    }

    CheckGttExpiry();

    // Reschedule: check proportionally to TTL to preserve radio silence in long-TTL networks
    Time interval = std::max(Seconds(1), m_gttTtl / 12);
    m_gttMaintenanceTimer.Schedule(interval);
}

void
RoutingProtocol::RreqRateLimitTimerExpire()
{
    NS_LOG_FUNCTION(this);
    m_rreqCount = 0;
    m_rreqRateLimitTimer.Schedule(Seconds(1));
}

void
RoutingProtocol::RerrRateLimitTimerExpire()
{
    NS_LOG_FUNCTION(this);
    m_rerrCount = 0;
    m_rerrRateLimitTimer.Schedule(Seconds(1));
}

void
RoutingProtocol::RouteRequestTimerExpire(Ipv4Address dst)
{
    NS_LOG_LOGIC(this);
    RoutingTableEntry toDst;
    if (m_routingTable.LookupValidRoute(dst, toDst))
    {
        SendPacketFromQueue(dst, toDst.GetRoute());
        NS_LOG_LOGIC("route to " << dst << " found");
        return;
    }

    if (toDst.GetRreqCnt() == m_rreqRetries)
    {
        NS_LOG_LOGIC("route discovery to " << dst << " has been attempted RreqRetries ("
                                           << m_rreqRetries << ") times with ttl "
                                           << m_netDiameter);
        m_addressReqTimer.erase(dst);
        m_routingTable.DeleteRoute(dst);
        NS_LOG_DEBUG("Route not found. Drop all packets with dst " << dst);
        m_queue.DropPacketWithDst(dst);
        return;
    }

    if (toDst.GetFlag() == IN_SEARCH)
    {
        NS_LOG_LOGIC("Resend RREQ to " << dst << " previous ttl " << toDst.GetHop());
        SendRequest(dst);
    }
    else
    {
        NS_LOG_DEBUG("Route down. Stop search. Drop packet with destination " << dst);
        m_addressReqTimer.erase(dst);
        m_routingTable.DeleteRoute(dst);
        m_queue.DropPacketWithDst(dst);
    }
}

void
RoutingProtocol::AckTimerExpire(Ipv4Address neighbor, Time blacklistTimeout)
{
    NS_LOG_FUNCTION(this);
    NS_LOG_DEBUG("RREP-ACK timeout for " << neighbor << " — blacklisting as unidirectional");
    m_routingTable.MarkLinkAsUnidirectional(neighbor, blacklistTimeout);
    m_pendingAckEvents.erase(neighbor);
}

void
RoutingProtocol::SyncTimerExpire()
{
    NS_LOG_FUNCTION(this);
    if (m_mentor == Ipv4Address())
    {
        // Mentor became unreachable — re-broadcast HELLO
        NS_LOG_DEBUG("Mentor unreachable. Re-broadcasting HELLO.");
        SendHello();
        return;
    }
    SendSyncPull(m_mentor);
}

// ============================================================================
// ScheduleRreqRetry
// ============================================================================

void
RoutingProtocol::ScheduleRreqRetry(Ipv4Address dst)
{
    NS_LOG_FUNCTION(this << dst);
    if (m_addressReqTimer.find(dst) == m_addressReqTimer.end())
    {
        Timer timer(Timer::CANCEL_ON_DESTROY);
        m_addressReqTimer[dst] = timer;
    }
    m_addressReqTimer[dst].SetFunction(&RoutingProtocol::RouteRequestTimerExpire, this);
    m_addressReqTimer[dst].Cancel();
    m_addressReqTimer[dst].SetArguments(dst);
    RoutingTableEntry rt;
    m_routingTable.LookupRoute(dst, rt);
    Time retry;
    if (rt.GetHop() < m_netDiameter)
    {
        retry = 2 * m_nodeTraversalTime * (rt.GetHop() + 2);
    }
    else
    {
        NS_ABORT_MSG_UNLESS(rt.GetRreqCnt() > 0, "Unexpected value for GetRreqCnt()");
        uint16_t backoffFactor = rt.GetRreqCnt() - 1;
        NS_LOG_LOGIC("Applying binary exponential backoff factor " << backoffFactor);
        retry = m_netTraversalTime * (1 << backoffFactor);
    }
    m_addressReqTimer[dst].Schedule(retry);
    NS_LOG_LOGIC("Scheduled RREQ retry in " << retry.As(Time::S));
}

// ============================================================================
// UpdateRouteLifeTime
// ============================================================================

bool
RoutingProtocol::UpdateRouteLifeTime(Ipv4Address addr, Time lifetime)
{
    NS_LOG_FUNCTION(this << addr << lifetime);
    RoutingTableEntry rt;
    if (m_routingTable.LookupRoute(addr, rt))
    {
        if (rt.GetFlag() == VALID)
        {
            NS_LOG_DEBUG("Updating VALID route");
            rt.SetRreqCnt(0);
            rt.SetLifeTime(std::max(lifetime, rt.GetLifeTime()));
            m_routingTable.Update(rt);
            return true;
        }
    }
    return false;
}

// ============================================================================
// UpdateRouteToNeighbor
// ============================================================================

void
RoutingProtocol::UpdateRouteToNeighbor(Ipv4Address sender, Ipv4Address receiver)
{
    NS_LOG_FUNCTION(this << "sender " << sender << " receiver " << receiver);
    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(sender, toNeighbor))
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(
            /*dev=*/dev,
            /*dst=*/sender,
            /*vSeqNo=*/false,
            /*seqNo=*/0,
            /*iface=*/m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
            /*hops=*/1,
            /*nextHop=*/sender,
            /*lifetime=*/m_activeRouteTimeout);
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        if (toNeighbor.GetValidSeqNo() && (toNeighbor.GetHop() == 1) &&
            (toNeighbor.GetOutputDevice() == dev))
        {
            toNeighbor.SetLifeTime(std::max(m_activeRouteTimeout, toNeighbor.GetLifeTime()));
            m_routingTable.Update(toNeighbor);
        }
        else
        {
            RoutingTableEntry newEntry(
                /*dev=*/dev,
                /*dst=*/sender,
                /*vSeqNo=*/false,
                /*seqNo=*/0,
                /*iface=*/m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                /*hops=*/1,
                /*nextHop=*/sender,
                /*lifetime=*/std::max(m_activeRouteTimeout, toNeighbor.GetLifeTime()));
            m_routingTable.Update(newEntry);
        }
    }
}

// ============================================================================
// Utility methods
// ============================================================================

const GlobalTopologyTable&
RoutingProtocol::GetGtt() const
{
    return m_gtt;
}

double
RoutingProtocol::GetGttAccuracy() const
{
    uint32_t totalEntries = m_gtt.GetTotalEntries();
    if (totalEntries == 0)
    {
        return 1.0;
    }
    uint32_t activeCount = m_gtt.NodeCount();
    return static_cast<double>(activeCount) / static_cast<double>(totalEntries);
}

void
RoutingProtocol::PrintRoutingTable(Ptr<OutputStreamWrapper> stream, Time::Unit unit) const
{
    *stream->GetStream() << "Node: " << m_ipv4->GetObject<Node>()->GetId()
                         << "; Time: " << Now().As(unit)
                         << ", Local time: " << m_ipv4->GetObject<Node>()->GetLocalTime().As(unit)
                         << ", TAVRN Routing table" << std::endl;

    m_routingTable.Print(stream, unit);
    *stream->GetStream() << std::endl;

    *stream->GetStream() << "GTT (Global Topology Table):" << std::endl;
    m_gtt.Print(stream);
    *stream->GetStream() << std::endl;
}

bool
RoutingProtocol::IsMyOwnAddress(Ipv4Address src)
{
    NS_LOG_FUNCTION(this << src);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ipv4InterfaceAddress iface = j->second;
        if (src == iface.GetLocal())
        {
            return true;
        }
    }
    return false;
}

Ptr<Ipv4Route>
RoutingProtocol::LoopbackRoute(const Ipv4Header& hdr, Ptr<NetDevice> oif) const
{
    NS_LOG_FUNCTION(this << hdr);
    NS_ASSERT(m_lo);
    Ptr<Ipv4Route> rt = Create<Ipv4Route>();
    rt->SetDestination(hdr.GetDestination());

    auto j = m_socketAddresses.begin();
    if (oif)
    {
        for (j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
        {
            Ipv4Address addr = j->second.GetLocal();
            int32_t interface = m_ipv4->GetInterfaceForAddress(addr);
            if (oif == m_ipv4->GetNetDevice(static_cast<uint32_t>(interface)))
            {
                rt->SetSource(addr);
                break;
            }
        }
    }
    else
    {
        rt->SetSource(j->second.GetLocal());
    }
    NS_ASSERT_MSG(rt->GetSource() != Ipv4Address(), "Valid TAVRN source address not found");
    rt->SetGateway(Ipv4Address("127.0.0.1"));
    rt->SetOutputDevice(m_lo);
    return rt;
}

Ptr<Socket>
RoutingProtocol::FindSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    NS_LOG_FUNCTION(this << addr);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        if (iface == addr)
        {
            return socket;
        }
    }
    Ptr<Socket> socket;
    return socket;
}

Ptr<Socket>
RoutingProtocol::FindSubnetBroadcastSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    NS_LOG_FUNCTION(this << addr);
    for (auto j = m_socketSubnetBroadcastAddresses.begin();
         j != m_socketSubnetBroadcastAddresses.end();
         ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        if (iface == addr)
        {
            return socket;
        }
    }
    Ptr<Socket> socket;
    return socket;
}

// ============================================================================
// BootstrapTimeoutExpire — Self-bootstrap if no SYNC_OFFER received
// ============================================================================

void
RoutingProtocol::BootstrapTimeoutExpire()
{
    NS_LOG_FUNCTION(this);

    // If already bootstrapped or already have a mentor, nothing to do
    if (m_isBootstrapped || m_mentor != Ipv4Address())
    {
        return;
    }

    NS_LOG_DEBUG("Bootstrap timeout — self-bootstrapping with empty GTT");
    m_isBootstrapped = true;

    if (!m_socketAddresses.empty())
    {
        Ipv4Address myAddr = m_socketAddresses.begin()->second.GetLocal();
        m_gtt.AddOrUpdateEntry(myAddr, m_seqNo, 0);
        SendTcUpdate(myAddr, TcUpdateHeader::NODE_JOIN);
    }
}

// ============================================================================
// SyncPullTimeoutExpire — Handle mentor death during sync
// ============================================================================

void
RoutingProtocol::SyncPullTimeoutExpire()
{
    NS_LOG_FUNCTION(this);

    m_syncPullRetries++;

    if (m_syncPullRetries >= 3)
    {
        // Give up on this mentor — reset and re-broadcast HELLO
        NS_LOG_DEBUG("SYNC_PULL timeout after " << m_syncPullRetries
                     << " retries. Clearing mentor and re-broadcasting HELLO.");
        m_mentor = Ipv4Address();
        m_isBootstrapped = false;
        m_syncPullRetries = 0;
        m_syncOfferCandidates.clear();

        // Re-schedule bootstrap timeout in case no one responds
        m_bootstrapTimer.Cancel();
        m_bootstrapTimer.SetFunction(&RoutingProtocol::BootstrapTimeoutExpire, this);
        m_bootstrapTimer.Schedule(Seconds(3));

        SendHello();
    }
    else
    {
        // Retry the SYNC_PULL
        NS_LOG_DEBUG("SYNC_PULL timeout, retry " << m_syncPullRetries);
        SendSyncPull(m_mentor);
    }
}

// ============================================================================
// OfferCollectionExpire — Pick best mentor from collected offers
// ============================================================================

void
RoutingProtocol::OfferCollectionExpire()
{
    NS_LOG_FUNCTION(this);

    if (m_syncOfferCandidates.empty() || m_isBootstrapped || m_mentor != Ipv4Address())
    {
        m_syncOfferCandidates.clear();
        return;
    }

    // Pick the candidate with the largest GTT size
    SyncOfferCandidate best = m_syncOfferCandidates[0];
    for (const auto& candidate : m_syncOfferCandidates)
    {
        if (candidate.gttSize > best.gttSize)
        {
            best = candidate;
        }
    }
    m_syncOfferCandidates.clear();

    m_mentor = best.mentorAddr;
    NS_LOG_DEBUG("Offer collection complete. Selected mentor " << m_mentor
                 << " with GTT size " << best.gttSize);

    // Begin paginated sync
    m_syncNextIndex = 0;
    SendSyncPull(m_mentor);
}

// ============================================================================
// AssignStreams
// ============================================================================

int64_t
RoutingProtocol::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_uniformRandomVariable->SetStream(stream);
    return 1;
}

// ============================================================================
// NotifyTxError — MAC layer feedback
// ============================================================================

void
RoutingProtocol::NotifyTxError(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu)
{
    // Only treat REACHED_RETRY_LIMIT as true node unreachability.
    // Other drop reasons (FAILED_ENQUEUE, EXPIRED_LIFETIME, QOS_OLD_PACKET) are
    // queue management drops that occur during normal congestion — NOT link failures.
    // Treating them as node death causes false NODE_LEAVE floods under high traffic,
    // which is the primary cause of GTT accuracy regression in larger networks.
    //
    // The neighbor purge + RERR generation MUST also be gated on reason.
    // Previously it fired unconditionally before the check, causing RERR churn
    // for queue-management drops that are not actual link failures.
    if (reason != WIFI_MAC_DROP_REACHED_RETRY_LIMIT)
    {
        NS_LOG_DEBUG("NotifyTxError: drop reason " << static_cast<int>(reason)
                     << " is not retry-limit — ignoring");
        return;
    }

    // Improvement 2: Resolve MAC→IP BEFORE ProcessTxError purges the neighbor entry.
    Mac48Address failedMac = mpdu->GetHeader().GetAddr1();
    Ipv4Address failedIp = m_nb.LookupIpAddress(failedMac);

    // Existing behavior: mark neighbor close, purge, fire E_RERR via link failure callback.
    m_nb.GetTxErrorCallback()(mpdu->GetHeader());

    // Improvement 2: Immediately propagate node death via TC-UPDATE(NODE_LEAVE).
    // This replaces the slow path (wait for GttTtl expiry → verification probes → NODE_LEAVE).
    // Only send TC-UPDATE if MarkDeparted actually changed state.
    if (failedIp != Ipv4Address())
    {
        bool wasDeparted = m_gtt.MarkDeparted(failedIp);
        if (wasDeparted)
        {
            NS_LOG_DEBUG("NotifyTxError: MAC retry limit reached for " << failedIp
                         << " — sending immediate TC-UPDATE(NODE_LEAVE)");
            SendTcUpdate(failedIp, TcUpdateHeader::NODE_LEAVE);
        }
        else
        {
            NS_LOG_DEBUG("NotifyTxError: " << failedIp
                         << " already departed or not in GTT — skipping TC-UPDATE");
        }
    }
}

// ============================================================================
// RecvPhyRxSniffer — capture real RSSI from WiFi PHY
// ============================================================================

void
RoutingProtocol::RecvPhyRxSniffer(Ptr<const Packet> packet,
                                   uint16_t channelFreq,
                                   WifiTxVector txVector,
                                   MpduInfo aMpdu,
                                   SignalNoiseDbm signalNoise,
                                   uint16_t staId)
{
    // Extract the WiFi MAC header to get the source MAC address.
    // MonitorSnifferRx packets require CreateFragment to avoid full copy.
    if (packet->GetSize() < 24)
    {
        return; // Too small for a MAC header
    }
    Ptr<Packet> hdrOnly = packet->CreateFragment(0, std::min(packet->GetSize(), 36u));
    WifiMacHeader macHdr;
    if (hdrOnly->PeekHeader(macHdr) == 0)
    {
        return; // Cannot parse MAC header
    }

    Mac48Address srcMac = macHdr.GetAddr2();
    if (srcMac.IsGroup())
    {
        return; // Ignore broadcast/multicast source
    }

    // Store the RSSI (signal strength in dBm) per source MAC
    m_macRssiCache[srcMac] = static_cast<double>(signalNoise.signal);
}

// ============================================================================
// LookupRssiForIp — resolve IP to MAC, look up cached RSSI
// ============================================================================

double
RoutingProtocol::LookupRssiForIp(Ipv4Address neighbor)
{
    // Use the Neighbors ARP cache to resolve IP -> MAC
    Mac48Address mac = m_nb.LookupMacAddress(neighbor);
    if (mac == Mac48Address::GetBroadcast() || mac == Mac48Address())
    {
        return -100.0; // Unknown
    }

    auto it = m_macRssiCache.find(mac);
    if (it != m_macRssiCache.end())
    {
        return it->second;
    }
    return -100.0; // No RSSI data available
}

// ============================================================================
// SendReplyAck — Send RREP_ACK for unidirectional link detection
// ============================================================================

void
RoutingProtocol::SendReplyAck(Ipv4Address neighbor)
{
    NS_LOG_FUNCTION(this << " to " << neighbor);
    ERrepAckHeader h;
    TypeHeader typeHeader(TAVRN_E_RREP_ACK);
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    packet->AddHeader(h);
    packet->AddHeader(typeHeader);
    RoutingTableEntry toNeighbor;
    m_routingTable.LookupRoute(neighbor, toNeighbor);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toNeighbor.GetInterface());
    NS_ASSERT(socket);
    m_controlOverheadTrace(packet->GetSize());
    socket->SendTo(packet, 0, InetSocketAddress(neighbor, TAVRN_PORT));
}

// ============================================================================
// RecvReplyAck — Process RREP_ACK (cancel ACK timer)
// ============================================================================

void
RoutingProtocol::RecvReplyAck(Ipv4Address neighbor)
{
    NS_LOG_FUNCTION(this);
    // Cancel the pending blacklist timer — link confirmed bidirectional
    auto it = m_pendingAckEvents.find(neighbor);
    if (it != m_pendingAckEvents.end())
    {
        Simulator::Cancel(it->second);
        m_pendingAckEvents.erase(it);
        NS_LOG_DEBUG("RREP-ACK received from " << neighbor << " — link confirmed symmetric");

        // Clear any existing blacklist state and mark route valid
        RoutingTableEntry rt;
        if (m_routingTable.LookupRoute(neighbor, rt))
        {
            if (rt.IsUnidirectional())
            {
                rt.SetUnidirectional(false);
            }
            rt.SetFlag(VALID);
            m_routingTable.Update(rt);
        }
    }
}

// ============================================================================
// CancelPendingSyncOffer — Cleanup callback for m_recentlyMentored
// ============================================================================

void
RoutingProtocol::CancelPendingSyncOffer(Ipv4Address mentee)
{
    NS_LOG_FUNCTION(this << mentee);
    m_recentlyMentored.erase(mentee);
}

} // namespace tavrn
} // namespace ns3

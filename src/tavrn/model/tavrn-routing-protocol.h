/*
 * TAVRN v2: Topology Aware Vicinity-Reactive Network
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN routing protocol — the central class that ties together all
 * sub-components: GTT (topology awareness), AODV-style reactive routing,
 * mentorship bootstrap, TC-UPDATE gossip, and piggybacked metadata.
 *
 * Structural pattern: mirrors ns-3 AODV RoutingProtocol, extended with
 * the TAVRN topology mechanisms described in TAVRN_v2.md.
 */

#ifndef TAVRN_ROUTING_PROTOCOL_H
#define TAVRN_ROUTING_PROTOCOL_H

#include "tavrn-dpd.h"
#include "tavrn-gtt.h"
#include "tavrn-id-cache.h"
#include "tavrn-neighbor.h"
#include "tavrn-packet.h"
#include "tavrn-rqueue.h"
#include "tavrn-rtable.h"

#include "ns3/ipv4-interface.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/mac48-address.h"
#include "ns3/node.h"
#include "ns3/output-stream-wrapper.h"
#include "ns3/random-variable-stream.h"
#include "ns3/traced-callback.h"
#include "ns3/timer.h"
#include "ns3/wifi-phy.h"

#include <map>
#include <set>
#include <vector>

namespace ns3
{

class WifiMpdu;
enum WifiMacDropReason : uint8_t;

namespace tavrn
{

/**
 * @ingroup tavrn
 *
 * @brief TAVRN routing protocol — topology-aware reactive routing for mesh networks.
 *
 * TAVRN occupies the space between pure reactive (AODV) and full proactive (OLSR)
 * protocols.  It provides:
 *
 *  - **Reactive routing**: AODV-based on-demand route discovery using enhanced
 *    E_RREQ / E_RREP / E_RERR messages with piggybacked topology metadata.
 *
 *  - **Topology awareness**: A Global Topology Table (GTT) per node enumerating
 *    all known nodes on the mesh.  Maintained through mentorship bootstrap,
 *    piggybacked metadata on existing traffic, dual-TTL soft/hard expiry, and
 *    TC-UPDATE gossip on topology changes.
 *
 * The protocol is tunable across a reactivity spectrum (0.2 -- 0.5) via
 * ns-3 Attributes:
 *   - Near-AODV (0.2): long GTT-TTL, late soft expiry, no periodic HELLO
 *   - Balanced  (0.35): default configuration
 *   - Near-OLSR (0.5): short GTT-TTL, early soft expiry, periodic HELLOs
 *
 * @see TAVRN_v2.md for the full protocol specification.
 * @see GlobalTopologyTable for the GTT data structure.
 */
class RoutingProtocol : public Ipv4RoutingProtocol
{
  public:
    /**
     * @brief Get the type ID.
     *
     * Registers all tunable parameters as ns-3 Attributes so they can be
     * set from simulation scripts via Config::Set or helper.Set().
     *
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    /// UDP port number used for all TAVRN control traffic.
    static const uint32_t TAVRN_PORT;

    /// Default constructor.
    RoutingProtocol();
    /// Destructor.
    ~RoutingProtocol() override;
    /// Release resources and cancel all pending timers.
    void DoDispose() override;

    // =========================================================================
    /// @name Ipv4RoutingProtocol interface
    /// Inherited virtual methods that integrate TAVRN into the ns-3 IP stack.
    ///@{

    /**
     * @brief Route an outbound packet.
     *
     * If a valid route exists, return it immediately.  Otherwise queue the
     * packet and initiate AODV-style route discovery (E_RREQ).
     *
     * @param p       the packet to route
     * @param header  the IPv4 header (destination address is key)
     * @param oif     preferred output interface (may be null)
     * @param sockerr [out] set to an error code on failure
     * @return a route to the destination, or nullptr if deferred/failed
     */
    Ptr<Ipv4Route> RouteOutput(Ptr<Packet> p,
                               const Ipv4Header& header,
                               Ptr<NetDevice> oif,
                               Socket::SocketErrno& sockerr) override;

    /**
     * @brief Route an inbound packet.
     *
     * Dispatches to local delivery, unicast forwarding, or multicast
     * forwarding as appropriate.  Also updates the GTT from piggybacked
     * topology metadata observed on forwarded traffic.
     *
     * @param p      the received packet
     * @param header the IPv4 header
     * @param idev   the input network device
     * @param ucb    callback for unicast forwarding
     * @param mcb    callback for multicast forwarding
     * @param lcb    callback for local delivery
     * @param ecb    callback for error reporting
     * @return true if the packet was consumed (forwarded or delivered)
     */
    bool RouteInput(Ptr<const Packet> p,
                    const Ipv4Header& header,
                    Ptr<const NetDevice> idev,
                    const UnicastForwardCallback& ucb,
                    const MulticastForwardCallback& mcb,
                    const LocalDeliverCallback& lcb,
                    const ErrorCallback& ecb) override;

    /**
     * @brief Notification that an interface has been brought up.
     *
     * Opens a raw UDP socket on the interface for TAVRN control traffic
     * and begins protocol operation if this is the first interface.
     *
     * @param interface the index of the interface
     */
    void NotifyInterfaceUp(uint32_t interface) override;

    /**
     * @brief Notification that an interface has been brought down.
     *
     * Closes the TAVRN control socket on this interface and cleans up
     * routes that used this interface.
     *
     * @param interface the index of the interface
     */
    void NotifyInterfaceDown(uint32_t interface) override;

    /**
     * @brief Notification that an address has been added to an interface.
     * @param interface the index of the interface
     * @param address   the new address
     */
    void NotifyAddAddress(uint32_t interface, Ipv4InterfaceAddress address) override;

    /**
     * @brief Notification that an address has been removed from an interface.
     * @param interface the index of the interface
     * @param address   the removed address
     */
    void NotifyRemoveAddress(uint32_t interface, Ipv4InterfaceAddress address) override;

    /**
     * @brief Receive the Ipv4 instance from the IP stack.
     *
     * Called once during node setup.  Stores the Ipv4 pointer for later
     * interface enumeration and socket creation.
     *
     * @param ipv4 the Ipv4 instance
     */
    void SetIpv4(Ptr<Ipv4> ipv4) override;

    /**
     * @brief Print the TAVRN routing table to an output stream.
     *
     * Prints both the AODV-style routing table and the GTT contents.
     *
     * @param stream the output stream wrapper
     * @param unit   the time unit to use (default Time::S)
     */
    void PrintRoutingTable(Ptr<OutputStreamWrapper> stream,
                           Time::Unit unit = Time::S) const override;
    ///@}

    // =========================================================================
    /// @name TAVRN-specific public API
    /// Methods exposed for application-layer queries and simulation helpers.
    ///@{

    /**
     * @brief Access the Global Topology Table for application-layer queries.
     *
     * Applications (e.g., service discovery layers like Matter or CoAP) can
     * use this to enumerate known nodes, check node existence, or query
     * last-seen timestamps.
     *
     * @return const reference to the GTT
     * @see GlobalTopologyTable for the full query API
     */
    const GlobalTopologyTable& GetGtt() const;

    /**
     * @brief Get GTT accuracy metric.
     *
     * Returns the fraction of GTT entries that are non-departed, giving
     * a measure of how "fresh" and accurate the topology view is.
     *
     * @return accuracy as a ratio in [0.0, 1.0], or 1.0 if the GTT is empty
     */
    double GetGttAccuracy() const;

    /**
     * @brief Assign a fixed random variable stream number to the random
     * variables used by this model.
     *
     * @param stream first stream index to use
     * @return the number of stream indices assigned by this model
     */
    int64_t AssignStreams(int64_t stream);
    ///@}

  protected:
    /**
     * @brief Initialize the protocol.
     *
     * Called after all attributes are set and the node is fully constructed.
     * Triggers Start() which opens sockets, initializes timers, and begins
     * the mentorship HELLO broadcast.
     */
    void DoInitialize() override;

  private:
    // =========================================================================
    /// @name MAC layer feedback
    ///@{

    /**
     * @brief Callback invoked when a WiFi MPDU transmission fails.
     *
     * Used to detect link breaks to next-hop neighbors, triggering RERR
     * generation and GTT updates.
     *
     * @param reason the reason the MPDU was dropped
     * @param mpdu   the dropped MPDU
     */
    void NotifyTxError(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu);
    ///@}

    // =========================================================================
    /// @name AODV-derived routing parameters (configurable via ns-3 Attributes)
    /// These mirror the standard AODV parameters and control the reactive
    /// routing behavior of TAVRN.
    ///@{

    /// Maximum number of retransmissions of RREQ with TTL = NetDiameter to discover a route.
    uint32_t m_rreqRetries;
    /// Initial TTL value for RREQ.
    uint16_t m_ttlStart;
    /// TTL increment for each attempt using the expanding ring search for RREQ dissemination.
    uint16_t m_ttlIncrement;
    /// Maximum TTL value for expanding ring search; TTL = NetDiameter is used beyond this value.
    uint16_t m_ttlThreshold;
    /// Maximum number of RREQ per second.
    uint16_t m_rreqRateLimit;
    /// Maximum number of RERR per second.
    uint16_t m_rerrRateLimit;
    /// Period of time during which a route is considered to be valid.
    Time m_activeRouteTimeout;
    /// Net diameter: maximum possible number of hops between two nodes in the network.
    uint32_t m_netDiameter;
    /**
     * @brief Conservative estimate of the average one-hop traversal time.
     *
     * Includes queuing delays, interrupt processing times, and transfer times.
     */
    Time m_nodeTraversalTime;
    /// Estimate of the average net traversal time.
    Time m_netTraversalTime;
    /// Estimate of maximum time needed to find a route in the network.
    Time m_pathDiscoveryTime;
    /// Value of lifetime field in RREP generated by this node.
    Time m_myRouteTimeout;
    /// The maximum number of packets that the routing protocol may buffer.
    uint32_t m_maxQueueLen;
    /// The maximum period of time that a routing protocol may buffer a packet.
    Time m_maxQueueTime;
    /// Indicates only the destination may respond to this RREQ.
    bool m_destinationOnly;
    /// Indicates whether a gratuitous RREP should be unicast to the node that originated the route discovery.
    bool m_gratuitousReply;
    /// Timeout buffer added to route lifetimes (AODV parity).
    uint16_t m_timeoutBuffer;
    /// Number of allowed missed HELLOs before declaring neighbor loss.
    uint8_t m_allowedHelloLoss;
    /// Enable/disable network connectivity broadcasts (AODV parity).
    bool m_enableBroadcast;
    /// Wait time for RREP_ACK before blacklisting (NextHopWait in AODV).
    Time m_nextHopWait;
    /// Duration a neighbor is blacklisted after unidirectional link detection.
    Time m_blackListTimeout;
    ///@}

    // =========================================================================
    /// @name TAVRN-specific GTT and topology parameters (configurable via ns-3 Attributes)
    /// These control the topology awareness behavior unique to TAVRN.
    ///@{

    /**
     * @brief GTT entry default time-to-live.
     *
     * Determines how long a GTT entry remains valid before hard expiry.
     * Tunable: 30s (near-OLSR) to 300s (near-AODV).  Default: 120s.
     */
    Time m_gttTtl;

    /**
     * @brief Fraction of GTT-TTL at which soft expiry triggers a freshness request.
     *
     * When a GTT entry's remaining TTL drops below (m_gttTtl * m_softExpiryThreshold),
     * the next outgoing packet will piggyback a freshness query for that node.
     * Tunable: 0.25 (late, near-AODV) to 0.75 (early, near-OLSR).  Default: 0.5.
     */
    double m_softExpiryThreshold;

    /**
     * @brief Whether periodic HELLO messages are enabled.
     *
     * When disabled (default), HELLOs are only sent during bootstrap.
     * When enabled, periodic HELLOs provide faster failure detection at the
     * cost of increased control overhead.
     */
    bool m_enablePeriodicHello;

    /**
     * @brief Interval between periodic HELLO broadcasts.
     *
     * Only used when m_enablePeriodicHello is true.  Default: 10s.
     */
    Time m_helloInterval;

    /**
     * @brief Number of GTT entries per SYNC_DATA page during mentorship bootstrap.
     *
     * Controls the pagination granularity of the mentorship sync.
     * Default: 15 entries per page.
     */
    uint32_t m_syncPageSize;

    /**
     * @brief Maximum number of GTT metadata entries piggybacked per message.
     *
     * Controls the per-message overhead of topology piggybacking (Principle 3).
     * Default: 5 entries.
     */
    uint8_t m_maxMetadataEntries;

    /**
     * @brief Cooldown period after piggybacking a GTT entry on a data packet.
     *
     * Prevents the same entry from being re-piggybacked until the cooldown
     * expires or the entry's state changes. Reduces redundant propagation
     * while ensuring topology changes are disseminated.
     * Default: 5s. Candidates: 1s, 5s, 10s.
     */
    Time m_piggybackCooldown;

    /**
     * @brief UUID expiry window for TC-UPDATE deduplication.
     *
     * UUIDs older than this window are purged from the dedup cache.
     * Should be >= 2x expected network diameter propagation time.
     */
    Time m_tcUpdateExpiryWindow;
    ///@}

    // =========================================================================
    /// @name Core protocol state
    ///@{

    /// IP protocol instance.
    Ptr<Ipv4> m_ipv4;
    /// Raw unicast socket per each IP interface, map socket -> iface address (IP + mask).
    std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketAddresses;
    /// Raw subnet directed broadcast socket per each IP interface, map socket -> iface address (IP + mask).
    std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketSubnetBroadcastAddresses;
    /// Loopback device used to defer RREQ until packet will be fully formed.
    Ptr<NetDevice> m_lo;
    ///@}

    // =========================================================================
    /// @name Sub-components
    /// Composition-based sub-components that implement specific protocol concerns.
    ///@{

    /// The Global Topology Table — TAVRN's core topology-awareness data structure.
    GlobalTopologyTable m_gtt;
    /// AODV-style routing table for next-hop resolution.
    RoutingTable m_routingTable;
    /// Drop-front queue for packets awaiting route discovery.
    RequestQueue m_queue;
    /// RREQ duplicate detection cache.
    IdCache m_rreqIdCache;
    /// Separate TC-UPDATE UUID deduplication cache with longer lifetime
    IdCache m_tcUuidCache;
    /// Neighbor management with per-neighbor RSSI caching for mentorship backoff.
    Neighbors m_nb;
    /// Broadcast duplicate packet detection (matches AODV's m_dpd).
    DuplicatePacketDetection m_dpd;

    /**
     * Improvement 2: Subject-based TC-UPDATE dedup cache.
     * Squashes redundant NODE_LEAVE floods when multiple neighbors detect the
     * same node death simultaneously. Keyed on {subject IP, event type} with
     * a 1-second expiry window.
     */
    std::map<std::pair<Ipv4Address, uint8_t>, Time> m_tcSubjectCache;
    ///@}

    // =========================================================================
    /// @name Sequence counters
    ///@{

    /// Broadcast ID: monotonically increasing RREQ identifier.
    uint32_t m_requestId;
    /// This node's own sequence number (for route freshness).
    uint32_t m_seqNo;
    /// Local TC-UPDATE sequence counter — combined with this node's address forms a 64-bit UUID.
    uint32_t m_tcUpdateSeqNo;
    /// Number of RREQs sent in the current rate-limit window.
    uint16_t m_rreqCount;
    /// Number of RERRs sent in the current rate-limit window.
    uint16_t m_rerrCount;
    ///@}

    // =========================================================================
    /// @name Mentorship state
    /// State variables for the TAVRN mentorship bootstrap protocol.
    /// @see TAVRN_v2.md Section "Initialization (HELLO / Mentorship)"
    ///@{

    /// True once this node has completed initial GTT synchronization from a mentor.
    bool m_isBootstrapped;
    /// IPv4 address of the current mentor (valid only during bootstrap).
    Ipv4Address m_mentor;
    /// Next page index to request from the mentor during paginated SYNC_PULL.
    uint32_t m_syncNextIndex;
    /// Timer for requesting the next sync page from the mentor.
    Timer m_syncTimer;

    // --- Bootstrap self-promotion timer ---
    /// Timer that fires if no SYNC_OFFER is received; triggers self-bootstrap.
    Timer m_bootstrapTimer;
    /// Callback when bootstrap timeout fires — self-bootstrap with empty GTT.
    void BootstrapTimeoutExpire();

    // --- SYNC_PULL timeout for mentor death recovery ---
    /// Timer for SYNC_PULL request timeout.
    Timer m_syncPullTimeout;
    /// Number of SYNC_PULL retries attempted.
    uint32_t m_syncPullRetries;
    /// Callback when SYNC_PULL times out — retry or find new mentor.
    void SyncPullTimeoutExpire();

    // --- Per-entry verification tracking for hard expiry ---
    /// Tracks verification state for hard-expired GTT entries.
    struct VerificationInfo
    {
        Time firstVerifyTime;  ///< When first verification E_RREQ was sent
        Time lastProbeTime;    ///< When last verification probe was actually sent
        uint32_t retryCount;   ///< Number of verification probes actually sent
    };
    /// Map of node addresses to their verification state.
    std::map<Ipv4Address, VerificationInfo> m_verificationState;

    // --- Dampening for redundant SYNC_OFFER ---
    /// Map of mentees to their cleanup EventIds (prevents re-offering; cancellable in DoDispose).
    std::map<Ipv4Address, EventId> m_recentlyMentored;

    // --- Mentor failover with collection window ---
    /// Candidate mentor from a SYNC_OFFER.
    struct SyncOfferCandidate
    {
        Ipv4Address mentorAddr; ///< Address of the offering mentor
        uint32_t gttSize;       ///< Size of the mentor's GTT
    };
    /// Collected SYNC_OFFER candidates during the offer collection window.
    std::vector<SyncOfferCandidate> m_syncOfferCandidates;
    /// Timer for the offer collection window.
    Timer m_offerCollectionTimer;
    /// Callback when offer collection window expires — pick best mentor.
    void OfferCollectionExpire();

    // --- Round-robin cursor for metadata piggybacking ---
    /// Cursor position for round-robin metadata slot filling.
    uint32_t m_metadataCursor;

    // --- Improvement 1: Conditional piggybacking state ---
    /// Round-robin cursor for conditional metadata (separate from control metadata).
    uint32_t m_conditionalCursor;
    /// Tracks when each GTT entry was last piggybacked on a data packet.
    /// Entries are not re-piggybacked until cooldown expires or state changes.
    std::map<Ipv4Address, Time> m_piggybackCooldownMap;

    // --- Smart TTL: Track destinations where GTT-assisted TTL was already attempted ---
    /// Destinations that have already used Smart TTL once; next retry forces m_netDiameter.
    std::set<Ipv4Address> m_smartTtlUsed;

    // --- Per-destination RREQ start time for route discovery latency ---
    /// Maps destination address to the time when the first RREQ was sent.
    std::map<Ipv4Address, Time> m_rreqStartTime;

    // --- Tracked pending SYNC_OFFER EventIds for broadcast dampening ---
    /// Maps mentee address to the scheduled SendSyncOffer EventId (cancellable by overheard offers).
    std::map<Ipv4Address, EventId> m_pendingSyncOfferEvents;
    /// Cancel a pending SYNC_OFFER for a mentee (called when we overhear another offer).
    void CancelPendingSyncOffer(Ipv4Address mentee);
    ///@}

    // =========================================================================
    /// @name Protocol startup
    ///@{

    /**
     * @brief Start protocol operation.
     *
     * Opens TAVRN control sockets on all active interfaces, initializes
     * timers, and broadcasts the initial HELLO ("I am new") message to
     * begin the mentorship process.
     */
    void Start();
    ///@}

    // =========================================================================
    /// @name AODV-based routing methods
    /// Core reactive routing mechanics inherited from the AODV design.
    ///@{

    /**
     * @brief Queue packet and initiate route discovery.
     *
     * Called when RouteOutput has no valid route.  Buffers the packet in
     * the request queue and triggers SendRequest() for the destination.
     *
     * @param p      the packet to route
     * @param header the IPv4 header
     * @param ucb    the UnicastForwardCallback function
     * @param ecb    the ErrorCallback function
     */
    void DeferredRouteOutput(Ptr<const Packet> p,
                             const Ipv4Header& header,
                             UnicastForwardCallback ucb,
                             ErrorCallback ecb);

    /**
     * @brief Forward a packet if a valid route exists.
     *
     * @param p      the packet to forward
     * @param header the IPv4 header
     * @param ucb    the UnicastForwardCallback function
     * @param ecb    the ErrorCallback function
     * @return true if the packet was forwarded
     */
    bool Forwarding(Ptr<const Packet> p,
                    const Ipv4Header& header,
                    UnicastForwardCallback ucb,
                    ErrorCallback ecb);

    /**
     * @brief Schedule RREQ retry using expanding ring search.
     *
     * Repeated attempts at route discovery for a single destination use
     * increasing TTL values.
     *
     * @param dst the destination IP address
     */
    void ScheduleRreqRetry(Ipv4Address dst);

    /**
     * @brief Update the lifetime of a routing table entry.
     *
     * Sets the lifetime to the maximum of the existing lifetime and lt.
     *
     * @param addr the destination address
     * @param lt   the proposed lifetime
     * @return true if the route to addr exists
     */
    bool UpdateRouteLifeTime(Ipv4Address addr, Time lt);

    /**
     * @brief Update the route to a direct neighbor.
     *
     * Called on every packet reception to maintain fresh next-hop entries.
     *
     * @param sender   IP address of the neighbor (packet source)
     * @param receiver IP address of the local receiving interface
     */
    void UpdateRouteToNeighbor(Ipv4Address sender, Ipv4Address receiver);

    /**
     * @brief Test whether the provided address is assigned to this node.
     *
     * @param src the IP address to check
     * @return true if src is one of this node's interface addresses
     */
    bool IsMyOwnAddress(Ipv4Address src);

    /**
     * @brief Find the unicast socket bound to the given interface address.
     *
     * @param iface the interface address
     * @return the socket, or nullptr if not found
     */
    Ptr<Socket> FindSocketWithInterfaceAddress(Ipv4InterfaceAddress iface) const;

    /**
     * @brief Find the subnet-directed broadcast socket bound to the given interface address.
     *
     * @param iface the interface address
     * @return the socket, or nullptr if not found
     */
    Ptr<Socket> FindSubnetBroadcastSocketWithInterfaceAddress(Ipv4InterfaceAddress iface) const;

    /**
     * @brief Create a loopback route for the given header.
     *
     * Used to defer route request processing until the packet is fully formed.
     *
     * @param header the IPv4 header
     * @param oif    the preferred output interface (may be null)
     * @return the loopback route
     */
    Ptr<Ipv4Route> LoopbackRoute(const Ipv4Header& header, Ptr<NetDevice> oif) const;
    ///@}

    // =========================================================================
    /// @name Receive handlers
    /// Methods that dispatch and process incoming TAVRN control messages.
    ///@{

    /**
     * @brief Main receive dispatcher for TAVRN control packets.
     *
     * Reads the TypeHeader to determine the message type and dispatches
     * to the appropriate Recv* handler.  Also extracts and processes
     * piggybacked TopologyMetadataHeader from E_RREQ/E_RREP/E_RERR.
     *
     * @param socket the receiving socket
     */
    void RecvTavrn(Ptr<Socket> socket);

    /**
     * @brief Process a received E_RREQ (Enhanced Route Request).
     *
     * Handles RREQ processing per AODV rules, plus processes piggybacked
     * topology metadata and updates the GTT.
     *
     * @param p        the packet (E_RREQ header already peeked)
     * @param receiver the local interface address that received the packet
     * @param src      the IP address of the sender
     */
    void RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src);

    /**
     * @brief Process a received E_RREP (Enhanced Route Reply).
     *
     * Handles RREP processing per AODV rules, plus processes piggybacked
     * topology metadata and updates the GTT.
     *
     * @param p   the packet
     * @param my  the local interface address that received the packet
     * @param src the IP address of the sender
     */
    void RecvReply(Ptr<Packet> p, Ipv4Address my, Ipv4Address src);

    /**
     * @brief Process a received E_RERR (Enhanced Route Error).
     *
     * Handles RERR processing per AODV rules, plus processes piggybacked
     * topology metadata and may trigger TC-UPDATE for departed nodes.
     *
     * @param p   the packet
     * @param src the IP address of the sender
     */
    void RecvError(Ptr<Packet> p, Ipv4Address src);

    /**
     * @brief Process a received HELLO message.
     *
     * If the HELLO indicates a new node (isNew flag), this triggers the
     * mentorship offer process: compute RSSI-proportional backoff and
     * potentially send a SYNC_OFFER.  Also updates GTT with the new node.
     *
     * @param p   the packet
     * @param src the IP address of the sender
     */
    void RecvHello(Ptr<Packet> p, Ipv4Address src);

    /**
     * @brief Process a received SYNC_OFFER from a potential mentor.
     *
     * If this node is not yet bootstrapped, accept the first offer and
     * begin paginated SYNC_PULL.  If already bootstrapped or already has
     * a mentor, ignore the offer (promiscuous dampening).
     *
     * @param p   the packet
     * @param src the IP address of the offering mentor
     */
    void RecvSyncOffer(Ptr<Packet> p, Ipv4Address src);

    /**
     * @brief Process a received SYNC_PULL request from a mentee.
     *
     * Extract the requested page range and respond with SYNC_DATA
     * containing the appropriate GTT entries.
     *
     * @param p   the packet
     * @param src the IP address of the requesting mentee
     */
    void RecvSyncPull(Ptr<Packet> p, Ipv4Address src);

    /**
     * @brief Process a received SYNC_DATA page from a mentor.
     *
     * Merge the received GTT entries into the local GTT using
     * GlobalTopologyTable::MergeEntry().  If more pages remain,
     * schedule the next SYNC_PULL.  If sync is complete, mark
     * this node as bootstrapped and broadcast a TC-UPDATE(JOIN).
     *
     * @param p   the packet
     * @param src the IP address of the mentor
     */
    void RecvSyncData(Ptr<Packet> p, Ipv4Address src);

    /**
     * @brief Send RREP_ACK to acknowledge receipt of an RREP with ACK flag.
     *
     * @param neighbor the neighbor that sent the RREP requesting an ACK
     */
    void SendReplyAck(Ipv4Address neighbor);

    /**
     * @brief Process a received E_RREP_ACK.
     *
     * Cancels the ACK timer for the neighbor, confirming the link is bidirectional.
     *
     * @param neighbor the neighbor that sent the ACK
     */
    void RecvReplyAck(Ipv4Address neighbor);

    /**
     * @brief Process a received TC-UPDATE broadcast.
     *
     * Check the UUID against the dedup cache.  If new:
     *  1. Update the local GTT (add on JOIN, mark departed on LEAVE)
     *  2. Mark UUID as seen in the IdCache
     *  3. Forward TC-UPDATE to all neighbors
     *
     * If the UUID was already seen, silently drop.
     *
     * @param p   the packet
     * @param src the IP address of the immediate sender (not necessarily the originator)
     */
    void RecvTcUpdate(Ptr<Packet> p, Ipv4Address src);
    ///@}

    // =========================================================================
    /// @name Send handlers
    /// Methods that construct and transmit TAVRN control messages.
    ///@{

    /**
     * @brief Send a packet to a destination via a specific socket.
     *
     * @param socket      the socket to send on
     * @param packet      the packet to send
     * @param destination the destination IP address
     */
    void SendTo(Ptr<Socket> socket, Ptr<Packet> packet, Ipv4Address destination);

    /**
     * @brief Forward a queued packet after route discovery completes.
     *
     * Dequeues all packets for the given destination and forwards them
     * using the discovered route.
     *
     * @param dst   the destination address
     * @param route the discovered route
     */
    void SendPacketFromQueue(Ipv4Address dst, Ptr<Ipv4Route> route);

    /**
     * @brief Initiate route discovery by broadcasting an E_RREQ.
     *
     * Constructs an ERreqHeader with topology metadata piggybacked via
     * BuildTopologyMetadata(), increments the request ID and sequence
     * number, and broadcasts to all interfaces.
     *
     * @param dst the destination address to discover a route to
     */
    void SendRequest(Ipv4Address dst);

    /**
     * @brief Send an E_RREP in response to a received E_RREQ.
     *
     * Called when this node is the destination or has a fresh enough route.
     * Includes piggybacked topology metadata.
     *
     * @param rreqHeader the received RREQ header
     * @param toOrigin   routing table entry toward the RREQ originator
     */
    void SendReply(const ERreqHeader& rreqHeader, const RoutingTableEntry& toOrigin);

    /**
     * @brief Send an E_RREP from an intermediate node with a valid cached route.
     *
     * @param toDst    routing table entry toward the destination
     * @param toOrigin routing table entry toward the RREQ originator
     * @param gratRep  if true, also send a gratuitous RREP to the destination
     */
    void SendReplyByIntermediateNode(RoutingTableEntry& toDst,
                                     RoutingTableEntry& toOrigin,
                                     bool gratRep);

    /**
     * @brief Initiate RERR when a link break to a next-hop neighbor is detected.
     *
     * Collects all destinations that used the broken next-hop and constructs
     * an ERerrHeader.  Also checks if the lost neighbor should be marked as
     * departed in the GTT.
     *
     * @param nextHop the unreachable next-hop address
     */
    void SendRerrWhenBreaksLinkToNextHop(Ipv4Address nextHop);

    /**
     * @brief Forward a RERR message to affected precursors.
     *
     * @param packet     the RERR packet to forward
     * @param precursors list of precursor addresses to notify
     */
    void SendRerrMessage(Ptr<Packet> packet, std::vector<Ipv4Address> precursors);

    /**
     * @brief Send RERR when there is no route to forward an input packet.
     *
     * Unicast if there is a reverse route to the originator, broadcast otherwise.
     *
     * @param dst      the unreachable destination address
     * @param dstSeqNo the destination sequence number
     * @param origin   the originating node address
     */
    void SendRerrWhenNoRouteToForward(Ipv4Address dst, uint32_t dstSeqNo, Ipv4Address origin);
    ///@}

    // =========================================================================
    /// @name TAVRN topology mechanism methods
    /// Methods implementing the TAVRN-specific topology awareness mechanisms.
    ///@{

    // --- Mentorship (Spec: Section "Initialization") ---

    /**
     * @brief Broadcast a HELLO message.
     *
     * For a new node: broadcasts "I am new" (isNew=true) to trigger
     * mentorship offers from neighbors.
     * For periodic keepalive: broadcasts "I am here" (isNew=false) to
     * maintain neighbor freshness (only when m_enablePeriodicHello is true).
     */
    void SendHello();

    /**
     * @brief Send a unicast SYNC_OFFER to a newly discovered node.
     *
     * Called after RSSI-proportional backoff when this node wins the
     * mentorship race.  Contains this node's GTT size so the mentee
     * knows how many pages to expect.
     *
     * @param mentee the IP address of the new node requesting mentorship
     */
    void SendSyncOffer(Ipv4Address mentee);

    /**
     * @brief Request the next page of GTT entries from the mentor.
     *
     * Sends a SYNC_PULL with (m_syncNextIndex, m_syncPageSize) to the
     * mentor to continue paginated bootstrap.
     *
     * @param mentor the IP address of the mentor
     */
    void SendSyncPull(Ipv4Address mentor);

    /**
     * @brief Send a page of GTT entries to a mentee.
     *
     * Extracts entries [startIndex, startIndex + count) from the GTT
     * via GlobalTopologyTable::GetEntriesPage() and packs them into
     * a SYNC_DATA message.
     *
     * @param mentee     the IP address of the mentee
     * @param startIndex the starting index in the GTT
     * @param count      the number of entries to send
     */
    void SendSyncData(Ipv4Address mentee, uint32_t startIndex, uint32_t count);

    // --- TC-UPDATE (Spec: Section "Topology Change Broadcast") ---

    /**
     * @brief Originate a TC-UPDATE broadcast for a topology change event.
     *
     * Constructs a TcUpdateHeader with a fresh UUID (this node's address +
     * m_tcUpdateSeqNo++), marks the UUID as seen locally, and broadcasts
     * to all neighbors.
     *
     * @param subjectAddr the IP address of the node that joined or left
     * @param event       NODE_JOIN or NODE_LEAVE
     */
    void SendTcUpdate(Ipv4Address subjectAddr, TcUpdateHeader::EventType event);

    /**
     * @brief Re-broadcast a TC-UPDATE received from another node.
     *
     * The UUID has already been checked for duplicates before calling this.
     *
     * @param p the TC-UPDATE packet to forward
     */
    void ForwardTcUpdate(Ptr<Packet> p, uint8_t ttl = 0);

    // --- Piggybacking (Spec: Principle 3) ---

    /**
     * @brief Build a TopologyMetadataHeader for piggybacking on outgoing traffic.
     *
     * Selects up to m_maxMetadataEntries GTT entries to include:
     *  - Prioritize entries at or near soft expiry (freshness requests)
     *  - Include recently learned entries for passive dissemination
     *
     * @return a TopologyMetadataHeader ready to append to E_RREQ/E_RREP/E_RERR
     */
    TopologyMetadataHeader BuildTopologyMetadata();

    /**
     * @brief Build metadata for data packets — conditional piggybacking (Improvement 1).
     *
     * Returns an empty header (0 entries) when the topology is stable (no
     * soft-expired entries and no entries needing propagation). This makes
     * data packets AODV-sized when piggybacking is not needed.
     *
     * When piggybacking IS needed:
     *  - Only soft-expired entries (freshness requests) are included
     *  - Capped at m_maxMetadataEntries per packet
     *  - Entries are marked with a cooldown to prevent redundant re-propagation
     *
     * @return a TopologyMetadataHeader (may be empty if nothing needs propagation)
     */
    TopologyMetadataHeader BuildConditionalMetadata();

    /**
     * @brief Process topology metadata received from a peer.
     *
     * For each entry in the metadata:
     *  - If it's a freshness request and we have significantly fresher info,
     *    schedule a freshness response
     *  - Update the local GTT if the received info is newer
     *
     * @param meta   the TopologyMetadataHeader to process
     * @param sender the IP address of the node that sent the metadata
     */
    void ProcessTopologyMetadata(const TopologyMetadataHeader& meta, Ipv4Address sender);

    // --- Soft-expiry refresh (Spec: "Steady State") ---

    /**
     * @brief Send a freshness response for a specific node.
     *
     * Called when processing a freshness request from a peer and this node
     * has significantly fresher information (TTL > 2x requester's TTL).
     * The response is piggybacked on the next outgoing message or sent
     * as a standalone HELLO if no traffic is pending.
     *
     * @param requester the IP address of the node requesting freshness
     * @param subject   the IP address of the node being queried about
     */
    void SendFreshnessResponse(Ipv4Address requester, Ipv4Address subject);

    /**
     * @brief Periodic GTT maintenance: check for soft and hard expiry.
     *
     * Iterates the GTT and:
     *  - For soft-expired entries: marks them for piggybacked freshness requests
     *  - For hard-expired entries: sends targeted E_RREQ to verify existence
     *  - For entries expired beyond 2x TTL with no response: marks departed,
     *    broadcasts TC-UPDATE(LEAVE)
     *  - Calls GlobalTopologyTable::Purge() to clean up old departed entries
     */
    void CheckGttExpiry();
    ///@}

    // =========================================================================
    /// @name Timers
    /// Protocol timers for periodic and deferred operations.
    ///@{

    /// Periodic HELLO timer (used when m_enablePeriodicHello is true).
    Timer m_htimer;
    /// GTT soft/hard expiry check timer.
    Timer m_gttMaintenanceTimer;
    /// RREQ rate-limit timer (resets m_rreqCount every second).
    Timer m_rreqRateLimitTimer;
    /// RERR rate-limit timer (resets m_rerrCount every second).
    Timer m_rerrRateLimitTimer;
    /// Per-destination RREQ retry timers (expanding ring search).
    std::map<Ipv4Address, Timer> m_addressReqTimer;

    /**
     * @brief Handle periodic HELLO timer expiration.
     *
     * If m_enablePeriodicHello is true, sends a HELLO and reschedules.
     * Also checks whether this node has recently broadcast (within the
     * hello interval) and suppresses the HELLO if so.
     */
    void HelloTimerExpire();

    /**
     * @brief Handle GTT maintenance timer expiration.
     *
     * Calls CheckGttExpiry() and reschedules the timer.
     */
    void GttMaintenanceTimerExpire();

    /**
     * @brief Reset RREQ count and reschedule the rate-limit timer (1 second interval).
     */
    void RreqRateLimitTimerExpire();

    /**
     * @brief Reset RERR count and reschedule the rate-limit timer (1 second interval).
     */
    void RerrRateLimitTimerExpire();

    /**
     * @brief Handle route request timer expiration for expanding ring search.
     *
     * Either retries the RREQ with a higher TTL or gives up and drops
     * queued packets for this destination.
     *
     * @param dst the destination address being searched for
     */
    void RouteRequestTimerExpire(Ipv4Address dst);

    /**
     * @brief Handle RREP_ACK timer expiration.
     *
     * If no ACK was received, mark the link to the neighbor as
     * unidirectional (blacklist) for the specified timeout.
     *
     * @param neighbor         the neighbor that failed to ACK
     * @param blacklistTimeout duration to blacklist the neighbor
     */
    void AckTimerExpire(Ipv4Address neighbor, Time blacklistTimeout);

    /**
     * @brief Handle sync pagination timer expiration.
     *
     * Sends the next SYNC_PULL request to the mentor.  If the mentor
     * becomes unreachable, falls back to re-broadcasting HELLO.
     */
    void SyncTimerExpire();
    ///@}

    // =========================================================================
    /// @name Tracing
    /// TracedCallback hooks for metrics collection in simulation scripts.
    ///@{

    /**
     * @brief Fired on every control packet transmission.
     *
     * Argument is the packet size in bytes.  Allows simulation scripts to
     * measure total TAVRN control overhead.
     */
    TracedCallback<uint32_t> m_controlOverheadTrace;

    /**
     * @brief Fired when a topology change has fully propagated.
     *
     * Arguments are the address of the changed node and the convergence
     * time (from TC-UPDATE origination to last node receiving it).
     */
    TracedCallback<Ipv4Address, Time> m_convergenceTrace;

    /**
     * @brief Fired when a route discovery completes.
     *
     * Arguments are the destination address and the discovery latency
     * (from initial RREQ send to RREP reception).
     */
    TracedCallback<Ipv4Address, Time> m_routeDiscoveryTrace;
    ///@}

    /// Timestamp of the last broadcast sent by this node (for HELLO suppression).
    Time m_lastBcastTime;

    // =========================================================================
    /// @name Real RSSI from WiFi PHY sniffer
    ///@{

    /**
     * @brief Callback for WiFi PHY MonitorSnifferRx trace.
     *
     * Extracts the source MAC address from the WiFi MAC header and
     * records the RSSI (signal strength in dBm) per MAC address.
     *
     * @param packet       the received packet (includes WiFi MAC header)
     * @param channelFreq  the channel frequency in MHz
     * @param txVector     TX parameters
     * @param aMpdu        A-MPDU info
     * @param signalNoise  signal and noise power in dBm
     * @param staId        STA-ID
     */
    void RecvPhyRxSniffer(Ptr<const Packet> packet,
                          uint16_t channelFreq,
                          WifiTxVector txVector,
                          MpduInfo aMpdu,
                          SignalNoiseDbm signalNoise,
                          uint16_t staId);

    /// Per-MAC-address RSSI cache populated by MonitorSnifferRx trace.
    std::map<Mac48Address, double> m_macRssiCache;

    /**
     * @brief Look up RSSI for a neighbor by IP address.
     *
     * Resolves the IP to a MAC address via the ARP caches maintained
     * by the Neighbors object, then looks up the MAC in m_macRssiCache.
     *
     * @param neighbor the IP address of the neighbor
     * @return the RSSI in dBm, or -100.0 if unknown
     */
    double LookupRssiForIp(Ipv4Address neighbor);
    ///@}

    /// Provides uniform random variables for jitter and backoff calculations.
    Ptr<UniformRandomVariable> m_uniformRandomVariable;
};

} // namespace tavrn
} // namespace ns3

#endif /* TAVRN_ROUTING_PROTOCOL_H */

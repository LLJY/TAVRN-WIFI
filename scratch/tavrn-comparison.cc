/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TAVRN Comparison Simulation Script
 *
 * Compares TAVRN against AODV, OLSR, and DSDV across 7 metrics:
 *   1. Control overhead (bytes) — via PHY-level TX trace
 *   2. Topology convergence time (TAVRN-specific)
 *   3. Packet delivery ratio
 *   4. End-to-end latency
 *   5. Route discovery latency (TAVRN-specific)
 *   6. GTT accuracy (TAVRN-specific)
 *   7. Energy consumption
 *
 * Scenarios:
 *   - "grid":    Static grid topology (primary benchmark)
 *   - "mobile":  Slow RandomWaypoint (stress test)
 *
 * Multi-seed sweep with 95% CI reporting
 *
 * Output: CSV to stdout for piping to data files.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/energy-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/wifi-co-trace-helper.h"
#include "ns3/wifi-radio-energy-model-helper.h"

// Protocol-specific helpers
#include "ns3/aodv-helper.h"
#include "ns3/dsdv-helper.h"
#include "ns3/olsr-helper.h"
#include "ns3/tavrn-helper.h"

// TAVRN internals for trace sources and GTT accuracy
#include "ns3/tavrn-routing-protocol.h"

// OLSR internals for topology table accuracy comparison
#include "ns3/olsr-routing-protocol.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <vector>

using namespace ns3;
using namespace ns3::energy;

NS_LOG_COMPONENT_DEFINE("TavrnComparison");

// =============================================================================
// Global metric accumulators (reset per run)
// =============================================================================

/// Total TAVRN-specific control overhead in bytes (from trace source).
static uint64_t g_tavrnControlBytes = 0;

/// Per-node convergence times observed from TAVRN trace source.
static std::vector<double> g_convergenceTimesMs;

/// Per-route discovery latencies observed from TAVRN trace source.
static std::vector<double> g_routeDiscoveryTimesMs;

/// Total bytes transmitted at PHY level across all nodes.
static uint64_t g_totalPhyTxBytes = 0;

/// Steady-state measurement: PHY bytes after bootstrap settles.
static uint64_t g_steadyStatePhyTxBytes = 0;

/// Time after which we consider the network "steady state" (set per run).
static double g_steadyStateStartSec = 0.0;

// =============================================================================
// Trace sink callbacks
// =============================================================================

/**
 * @brief Trace sink for TAVRN control overhead.
 */
static void
TavrnControlOverheadSink(uint32_t packetSize)
{
    g_tavrnControlBytes += packetSize;
}

/**
 * @brief Trace sink for TAVRN topology convergence time.
 */
static void
TavrnConvergenceSink(Ipv4Address addr, Time convergenceTime)
{
    g_convergenceTimesMs.push_back(convergenceTime.GetMilliSeconds());
}

/**
 * @brief Trace sink for TAVRN route discovery latency.
 */
static void
TavrnRouteDiscoverySink(Ipv4Address dst, Time latency)
{
    g_routeDiscoveryTimesMs.push_back(latency.GetMilliSeconds());
}

/**
 * @brief PHY-level TX sniffer — counts ALL transmitted bytes.
 *
 * Connected to MonitorSnifferTx on every node's WifiPhy.
 * This captures all frames including broadcast control traffic that
 * FlowMonitor misses, providing a symmetric overhead measurement
 * across all protocols.
 *
 * @param packet       the packet being transmitted (includes MAC header)
 * @param channelFreq  the channel frequency in MHz
 * @param txVector     TX parameters
 * @param aMpdu        A-MPDU info
 * @param staId        STA-ID
 */
static void
PhyTxSnifferSink(Ptr<const Packet> packet,
                 uint16_t channelFreq,
                 WifiTxVector txVector,
                 MpduInfo aMpdu,
                 uint16_t staId)
{
    uint32_t sz = packet->GetSize();
    g_totalPhyTxBytes += sz;
    if (Simulator::Now().GetSeconds() >= g_steadyStateStartSec)
    {
        g_steadyStatePhyTxBytes += sz;
    }
}

// =============================================================================
// Helper: Connect TAVRN trace sources on all nodes
// =============================================================================

static void
ConnectTavrnTraces(NodeContainer& nodes)
{
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
        if (!ipv4)
        {
            continue;
        }

        Ptr<Ipv4RoutingProtocol> proto = ipv4->GetRoutingProtocol();
        if (!proto)
        {
            continue;
        }

        Ptr<tavrn::RoutingProtocol> tavrnProto;
        tavrnProto = DynamicCast<tavrn::RoutingProtocol>(proto);

        if (!tavrnProto)
        {
            Ptr<Ipv4ListRouting> listRouting = DynamicCast<Ipv4ListRouting>(proto);
            if (listRouting)
            {
                for (uint32_t j = 0; j < listRouting->GetNRoutingProtocols(); ++j)
                {
                    int16_t priority;
                    Ptr<Ipv4RoutingProtocol> rp = listRouting->GetRoutingProtocol(j, priority);
                    tavrnProto = DynamicCast<tavrn::RoutingProtocol>(rp);
                    if (tavrnProto)
                    {
                        break;
                    }
                }
            }
        }

        if (tavrnProto)
        {
            tavrnProto->TraceConnectWithoutContext("ControlOverhead",
                                                   MakeCallback(&TavrnControlOverheadSink));
            tavrnProto->TraceConnectWithoutContext("TopologyConvergence",
                                                   MakeCallback(&TavrnConvergenceSink));
            tavrnProto->TraceConnectWithoutContext("RouteDiscovery",
                                                   MakeCallback(&TavrnRouteDiscoverySink));
        }
    }
}

// =============================================================================
// Connect PHY-level TX sniffer on all nodes (protocol-agnostic)
// =============================================================================

/**
 * @brief Connect MonitorSnifferTx on every node's WifiPhy.
 *
 * This is the uniform overhead measurement method: counts ALL bytes
 * transmitted at the PHY level regardless of protocol.
 */
static void
ConnectPhyTxTraces(NetDeviceContainer& wifiDevices)
{
    for (uint32_t i = 0; i < wifiDevices.GetN(); ++i)
    {
        Ptr<WifiNetDevice> wifi = DynamicCast<WifiNetDevice>(wifiDevices.Get(i));
        if (!wifi)
        {
            continue;
        }
        Ptr<WifiPhy> phy = wifi->GetPhy();
        if (phy)
        {
            phy->TraceConnectWithoutContext("MonitorSnifferTx",
                                           MakeCallback(&PhyTxSnifferSink));
        }
    }
}

// =============================================================================
// Helper: Compute average GTT accuracy across all nodes
// =============================================================================

static double
ComputeAverageGttAccuracy(NodeContainer& nodes)
{
    double sumAccuracy = 0.0;
    uint32_t count = 0;

    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
        if (!ipv4)
        {
            continue;
        }

        Ptr<Ipv4RoutingProtocol> proto = ipv4->GetRoutingProtocol();
        if (!proto)
        {
            continue;
        }

        Ptr<tavrn::RoutingProtocol> tavrnProto;
        tavrnProto = DynamicCast<tavrn::RoutingProtocol>(proto);

        if (!tavrnProto)
        {
            Ptr<Ipv4ListRouting> listRouting = DynamicCast<Ipv4ListRouting>(proto);
            if (listRouting)
            {
                for (uint32_t j = 0; j < listRouting->GetNRoutingProtocols(); ++j)
                {
                    int16_t priority;
                    Ptr<Ipv4RoutingProtocol> rp = listRouting->GetRoutingProtocol(j, priority);
                    tavrnProto = DynamicCast<tavrn::RoutingProtocol>(rp);
                    if (tavrnProto)
                    {
                        break;
                    }
                }
            }
        }

        if (tavrnProto)
        {
            sumAccuracy += tavrnProto->GetGttAccuracy();
            ++count;
        }
    }

    return (count > 0) ? (sumAccuracy / count) : 0.0;
}

// =============================================================================
// ComputeOlsrTopologyAccuracy — measure OLSR's topology knowledge
// =============================================================================
//
// For fair comparison with TAVRN's GTT accuracy, we measure what fraction of
// alive nodes each OLSR node knows about via its combined topology state:
//   - NeighborSet (1-hop, from HELLOs)
//   - TwoHopNeighborSet (2-hop, from HELLOs)
//   - TopologySet (multi-hop, from TC messages)
//
// accuracy_i = |known_alive_nodes_i| / (total_alive - 1)   [excluding self]
// result = average over all alive nodes

double
ComputeOlsrTopologyAccuracy(NodeContainer nodes)
{
    // Step 1: Build ground truth — set of all alive node addresses
    std::set<Ipv4Address> aliveAddrs;
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
        if (!ipv4 || !ipv4->IsUp(1))
        {
            continue; // interface down = failed node
        }
        aliveAddrs.insert(ipv4->GetAddress(1, 0).GetLocal());
    }

    if (aliveAddrs.size() <= 1)
    {
        return 1.0;
    }

    double sumAccuracy = 0.0;
    uint32_t count = 0;

    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
        if (!ipv4 || !ipv4->IsUp(1))
        {
            continue;
        }
        Ipv4Address myAddr = ipv4->GetAddress(1, 0).GetLocal();

        // Find OLSR routing protocol instance
        Ptr<olsr::RoutingProtocol> olsrProto;
        Ptr<Ipv4RoutingProtocol> proto = ipv4->GetRoutingProtocol();
        if (proto)
        {
            olsrProto = DynamicCast<olsr::RoutingProtocol>(proto);
            if (!olsrProto)
            {
                Ptr<Ipv4ListRouting> listRouting = DynamicCast<Ipv4ListRouting>(proto);
                if (listRouting)
                {
                    for (uint32_t j = 0; j < listRouting->GetNRoutingProtocols(); ++j)
                    {
                        int16_t priority;
                        Ptr<Ipv4RoutingProtocol> rp =
                            listRouting->GetRoutingProtocol(j, priority);
                        olsrProto = DynamicCast<olsr::RoutingProtocol>(rp);
                        if (olsrProto)
                        {
                            break;
                        }
                    }
                }
            }
        }

        if (!olsrProto)
        {
            continue;
        }

        const olsr::OlsrState& state = olsrProto->GetOlsrState();

        // Collect all unique node addresses known to this OLSR node
        std::set<Ipv4Address> knownAddrs;

        // 1-hop neighbors
        for (const auto& nb : state.GetNeighbors())
        {
            knownAddrs.insert(nb.neighborMainAddr);
        }

        // 2-hop neighbors
        for (const auto& twohop : state.GetTwoHopNeighbors())
        {
            knownAddrs.insert(twohop.neighborMainAddr);
            knownAddrs.insert(twohop.twoHopNeighborAddr);
        }

        // Multi-hop topology (TC entries)
        for (const auto& tc : state.GetTopologySet())
        {
            knownAddrs.insert(tc.destAddr);
            knownAddrs.insert(tc.lastAddr);
        }

        // Remove self
        knownAddrs.erase(myAddr);

        // Count how many known addresses are actually alive
        uint32_t knownAlive = 0;
        for (const auto& addr : knownAddrs)
        {
            if (aliveAddrs.count(addr) > 0)
            {
                ++knownAlive;
            }
        }

        double nodeAccuracy =
            static_cast<double>(knownAlive) / static_cast<double>(aliveAddrs.size() - 1);
        sumAccuracy += nodeAccuracy;
        ++count;
    }

    return (count > 0) ? (sumAccuracy / count) : 0.0;
}

// =============================================================================
// Per-run result structure
// =============================================================================

struct RunResult
{
    double pdr;
    double avgLatencyMs;
    uint64_t totalOverheadBytes;       // Full simulation PHY overhead
    uint64_t bootstrapOverheadBytes;   // PHY bytes during bootstrap phase only
    double steadyStateOverheadBps;     // Control overhead bytes/s during steady state
    double energyJ;                    // Total energy (incl. idle) — for convention
    double txEnergyJ;                  // TX-only energy (active radio cost)
    double rxEnergyJ;                  // RX-only energy (active radio cost)
    double gttAccuracy; // -1.0 for non-TAVRN
    double avgConvergenceMs;
    double avgRouteDiscoveryMs;
};

// =============================================================================
// Single simulation run
// =============================================================================

static RunResult
RunSingleSimulation(const std::string& protocol,
                    const std::string& scenario,
                    uint32_t nNodes,
                    double simTime,
                    uint32_t nFlows,
                    double areaSize,
                    uint32_t seed,
                    uint16_t appPort,
                    uint32_t packetSize,
                    double dataRate,
                    double maxSpeed,
                    double pauseTime,
                    uint32_t failNodes,
                    double failTime,
                    double rejoinTime,
                    double onTime,
                    double offTime,
                    uint32_t churnNodes,
                    double churnInterval,
                    bool hubTraffic,
                    bool verbose)
{
    RunResult result{};

    // Reset global accumulators
    g_tavrnControlBytes = 0;
    g_convergenceTimesMs.clear();
    g_routeDiscoveryTimesMs.clear();
    g_totalPhyTxBytes = 0;
    g_steadyStatePhyTxBytes = 0;
    g_steadyStateStartSec = 0.0; // Will be set below after appStartBase is defined

    // Configure random seed for reproducibility
    SeedManager::SetSeed(seed);
    SeedManager::SetRun(seed);

    if (verbose)
    {
        LogComponentEnable("TavrnComparison", LOG_LEVEL_INFO);
        if (protocol == "TAVRN")
        {
            LogComponentEnable("TavrnRoutingProtocol", LOG_LEVEL_WARN);
        }
    }

    // -------------------------------------------------------------------------
    // Create nodes
    // -------------------------------------------------------------------------
    NodeContainer nodes;
    nodes.Create(nNodes);

    // -------------------------------------------------------------------------
    // WiFi setup: ad-hoc, 802.11a, constant rate 6 Mbps
    // -------------------------------------------------------------------------
    WifiMacHelper wifiMac;
    wifiMac.SetType("ns3::AdhocWifiMac");

    YansWifiPhyHelper wifiPhy;
    YansWifiChannelHelper wifiChannel;
    wifiChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    wifiChannel.AddPropagationLoss("ns3::FriisPropagationLossModel");
    wifiPhy.SetChannel(wifiChannel.Create());

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211a);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue("OfdmRate6Mbps"),
                                 "ControlMode",
                                 StringValue("OfdmRate6Mbps"),
                                 "RtsCtsThreshold",
                                 UintegerValue(0));

    NetDeviceContainer wifiDevices = wifi.Install(wifiPhy, wifiMac, nodes);

    // -------------------------------------------------------------------------
    // Mobility — scenario-based
    // -------------------------------------------------------------------------
    if (scenario == "grid")
    {
        // Static grid: sqrt(nNodes) x sqrt(nNodes) with spacing to fill areaSize
        uint32_t gridSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(nNodes))));
        double spacing = areaSize / std::max(gridSide - 1, 1u);

        MobilityHelper mobility;
        mobility.SetPositionAllocator("ns3::GridPositionAllocator",
                                       "MinX", DoubleValue(0.0),
                                       "MinY", DoubleValue(0.0),
                                       "DeltaX", DoubleValue(spacing),
                                       "DeltaY", DoubleValue(spacing),
                                       "GridWidth", UintegerValue(gridSide),
                                       "LayoutType", StringValue("RowFirst"));
        mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        mobility.Install(nodes);
    }
    else if (scenario == "linear")
    {
        // Linear chain: all nodes in a line, spacing = areaSize / (n-1)
        double spacing = areaSize / std::max(nNodes - 1, 1u);
        Ptr<ListPositionAllocator> posAlloc = CreateObject<ListPositionAllocator>();
        for (uint32_t i = 0; i < nNodes; ++i)
        {
            posAlloc->Add(Vector(i * spacing, 0.0, 0.0));
        }
        MobilityHelper mobility;
        mobility.SetPositionAllocator(posAlloc);
        mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        mobility.Install(nodes);
    }
    else if (scenario == "home")
    {
        // Smart-home topology: rooms (clusters) connected by corridor nodes.
        // Creates chokepoint topology where nodes within a room are well-
        // connected but inter-room traffic must traverse bridge nodes.
        //
        // Layout: rooms arranged in a 2-row grid. Each room is a ~30m cluster.
        // Room centers spaced 100m apart (at WiFi range boundary).
        // 1-2 corridor nodes placed between adjacent rooms.
        // Node 0 (hub/gateway) placed at the center of the layout.

        std::mt19937 homeRng(seed + 7777); // deterministic per run

        // Decide room count: ~4-5 nodes per room, minimum 3 rooms
        uint32_t nRooms = std::max(3u, nNodes / 5);
        if (nRooms > 10) nRooms = 10; // cap for sanity

        // Room layout: 2 rows, nCols columns
        uint32_t nCols = (nRooms + 1) / 2;
        double roomSpacing = 100.0; // meters between room centers
        double roomRadius = 15.0;   // nodes scatter within +/-15m of room center

        // Compute room center positions
        struct RoomInfo {
            double cx, cy;
        };
        std::vector<RoomInfo> rooms(nRooms);
        for (uint32_t r = 0; r < nRooms; ++r)
        {
            uint32_t col = r % nCols;
            uint32_t row = r / nCols;
            rooms[r].cx = col * roomSpacing;
            rooms[r].cy = row * roomSpacing;
        }

        // Corridor nodes: place 1 bridge node between each pair of adjacent rooms
        // (horizontally and vertically adjacent in the grid)
        struct CorridorNode {
            double x, y;
        };
        std::vector<CorridorNode> corridors;
        for (uint32_t r = 0; r < nRooms; ++r)
        {
            uint32_t col = r % nCols;
            // Right neighbor
            if (col + 1 < nCols && r + 1 < nRooms)
            {
                corridors.push_back({(rooms[r].cx + rooms[r+1].cx) / 2.0,
                                      rooms[r].cy});
            }
            // Below neighbor
            uint32_t below = r + nCols;
            if (below < nRooms)
            {
                corridors.push_back({rooms[r].cx,
                                     (rooms[r].cy + rooms[below].cy) / 2.0});
            }
        }

        // Distribute nodes: node 0 (hub) at center, then room devices
        // (low indices = flow sources with hubTraffic), corridor nodes last.
        Ptr<ListPositionAllocator> posAlloc = CreateObject<ListPositionAllocator>();

        // Node 0 = hub at layout center
        double layoutCx = (nCols - 1) * roomSpacing / 2.0;
        double layoutCy = (nRooms > nCols ? roomSpacing : 0.0) / 2.0;
        posAlloc->Add(Vector(layoutCx, layoutCy, 0.0));

        uint32_t placed = 1; // node 0 is placed

        // Reserve slots for corridor nodes (placed last, high indices)
        uint32_t nCorridorNodes = std::min(static_cast<uint32_t>(corridors.size()),
                                            nNodes - placed);
        uint32_t nRoomNodes = nNodes - placed - nCorridorNodes;

        // Place room devices first (low indices = realistic flow sources)
        std::uniform_real_distribution<double> roomDist(-roomRadius, roomRadius);
        uint32_t roomIdx = 0;
        for (uint32_t i = 0; i < nRoomNodes; ++i, ++placed)
        {
            double x = rooms[roomIdx].cx + roomDist(homeRng);
            double y = rooms[roomIdx].cy + roomDist(homeRng);
            posAlloc->Add(Vector(x, y, 0.0));
            roomIdx = (roomIdx + 1) % nRooms;
        }

        // Place corridor/bridge nodes last (high indices, not flow endpoints)
        std::uniform_real_distribution<double> jitter(-3.0, 3.0);
        for (uint32_t c = 0; c < nCorridorNodes && placed < nNodes; ++c, ++placed)
        {
            posAlloc->Add(Vector(corridors[c].x + jitter(homeRng),
                                  corridors[c].y + jitter(homeRng), 0.0));
        }

        MobilityHelper mobility;
        mobility.SetPositionAllocator(posAlloc);
        mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        mobility.Install(nodes);
    }
    else // "mobile" — RandomWaypoint with configurable speed and pause
    {
        ObjectFactory posFactory;
        posFactory.SetTypeId("ns3::RandomRectanglePositionAllocator");

        std::ostringstream xStream;
        xStream << "ns3::UniformRandomVariable[Min=0.0|Max=" << areaSize << "]";
        std::ostringstream yStream;
        yStream << "ns3::UniformRandomVariable[Min=0.0|Max=" << areaSize << "]";

        posFactory.Set("X", StringValue(xStream.str()));
        posFactory.Set("Y", StringValue(yStream.str()));

        Ptr<PositionAllocator> positionAlloc =
            posFactory.Create()->GetObject<PositionAllocator>();

        std::ostringstream speedStream;
        speedStream << "ns3::UniformRandomVariable[Min=0.0|Max=" << maxSpeed << "]";
        std::ostringstream pauseStream;
        pauseStream << "ns3::ConstantRandomVariable[Constant=" << pauseTime << "]";

        MobilityHelper mobility;
        mobility.SetPositionAllocator(positionAlloc);
        mobility.SetMobilityModel(
            "ns3::RandomWaypointMobilityModel",
            "Speed",
            StringValue(speedStream.str()),
            "Pause",
            StringValue(pauseStream.str()),
            "PositionAllocator",
            PointerValue(positionAlloc));
        mobility.Install(nodes);
    }

    // -------------------------------------------------------------------------
    // Energy model: BasicEnergySource + WifiRadioEnergyModel on every node
    // -------------------------------------------------------------------------
    BasicEnergySourceHelper basicSourceHelper;
    basicSourceHelper.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(100.0));
    basicSourceHelper.Set("BasicEnergySupplyVoltageV", DoubleValue(3.3));

    EnergySourceContainer energySources = basicSourceHelper.Install(nodes);

    WifiRadioEnergyModelHelper radioEnergyHelper;
    radioEnergyHelper.Set("TxCurrentA", DoubleValue(0.380));
    radioEnergyHelper.Set("RxCurrentA", DoubleValue(0.313));
    radioEnergyHelper.Set("IdleCurrentA", DoubleValue(0.273));
    radioEnergyHelper.Set("SleepCurrentA", DoubleValue(0.033));

    DeviceEnergyModelContainer deviceEnergyModels =
        radioEnergyHelper.Install(wifiDevices, energySources);

    // -------------------------------------------------------------------------
    // Internet stack + routing protocol selection
    // -------------------------------------------------------------------------
    InternetStackHelper stack;

    if (protocol == "TAVRN")
    {
        TavrnHelper tavrn;
        tavrn.Set("GttTtl", TimeValue(Seconds(300)));
        tavrn.Set("ActiveRouteTimeout", TimeValue(Seconds(600)));
        tavrn.Set("SoftExpiryThreshold", DoubleValue(0.5));
        tavrn.Set("EnablePeriodicHello", BooleanValue(true));
        tavrn.Set("HelloInterval", TimeValue(Seconds(150)));
        tavrn.Set("MaxMetadataEntries", UintegerValue(4));
        stack.SetRoutingHelper(tavrn);
    }
    else if (protocol == "AODV")
    {
        AodvHelper aodv;
        stack.SetRoutingHelper(aodv);
    }
    else if (protocol == "OLSR")
    {
        OlsrHelper olsr;
        stack.SetRoutingHelper(olsr);
    }
    else if (protocol == "DSDV")
    {
        DsdvHelper dsdv;
        dsdv.Set("PeriodicUpdateInterval", TimeValue(Seconds(15)));
        dsdv.Set("SettlingTime", TimeValue(Seconds(6)));
        stack.SetRoutingHelper(dsdv);
    }
    else
    {
        NS_FATAL_ERROR("Unknown protocol: " << protocol
                                             << ". Use TAVRN, AODV, OLSR, or DSDV.");
    }

    stack.Install(nodes);

    // -------------------------------------------------------------------------
    // IP addressing
    // -------------------------------------------------------------------------
    Ipv4AddressHelper address;
    address.SetBase("10.0.0.0", "255.255.0.0");
    Ipv4InterfaceContainer interfaces = address.Assign(wifiDevices);

    // -------------------------------------------------------------------------
    // Node failure / rejoin scheduling
    // -------------------------------------------------------------------------
    if (failNodes > 0)
    {
        // Fail nodes from the center of the network (most disruptive)
        uint32_t startIdx = nNodes / 2 - failNodes / 2;
        for (uint32_t i = 0; i < failNodes && (startIdx + i) < nNodes; ++i)
        {
            uint32_t nodeIdx = startIdx + i;
            Ptr<Ipv4> nodeIpv4 = nodes.Get(nodeIdx)->GetObject<Ipv4>();
            // SetDown on interface 1 (the wifi interface; 0 is loopback)
            Simulator::Schedule(Seconds(failTime),
                                &Ipv4::SetDown, nodeIpv4, static_cast<uint32_t>(1));
            if (rejoinTime > 0.0)
            {
                Simulator::Schedule(Seconds(rejoinTime),
                                    &Ipv4::SetUp, nodeIpv4, static_cast<uint32_t>(1));
            }
        }
    }

    // -------------------------------------------------------------------------
    // Rolling churn: every churnInterval seconds, take down churnNodes random
    // non-flow-endpoint nodes and bring the previous batch back up.
    // This simulates transient topology with nodes entering/leaving like cars.
    // -------------------------------------------------------------------------
    if (churnNodes > 0 && churnNodes < nNodes)
    {
        // Build pool of churn-eligible node indices (exclude flow endpoints)
        // Replicate the exact endpoint logic from flow setup to protect only
        // the actual source/sink nodes.
        uint32_t af = std::min(nFlows, nNodes / 2);
        std::set<uint32_t> flowEndpoints;
        for (uint32_t f = 0; f < af; ++f)
        {
            uint32_t srcIdx, dstIdx;
            if (hubTraffic)
            {
                if (f < af / 2 + af % 2)
                {
                    srcIdx = f + 1;
                    if (srcIdx >= nNodes) srcIdx = nNodes - 1;
                    dstIdx = 0;
                }
                else
                {
                    uint32_t peerF = f - (af / 2 + af % 2);
                    srcIdx = 1 + peerF * 2;
                    dstIdx = 2 + peerF * 2;
                    if (srcIdx >= nNodes) srcIdx = nNodes - 2;
                    if (dstIdx >= nNodes) dstIdx = nNodes - 1;
                }
            }
            else
            {
                srcIdx = f;
                dstIdx = nNodes - 1 - f;
            }
            if (srcIdx == dstIdx)
            {
                dstIdx = (dstIdx + 1) % nNodes;
            }
            flowEndpoints.insert(srcIdx);
            flowEndpoints.insert(dstIdx);
        }
        std::vector<uint32_t> churnPool;
        for (uint32_t i = 0; i < nNodes; ++i)
        {
            if (flowEndpoints.find(i) == flowEndpoints.end())
            {
                churnPool.push_back(i);
            }
        }

        // Schedule churn rotations throughout the simulation
        // Start after bootstrap phase (120s) to let protocols stabilize
        double churnStart = 120.0;
        std::mt19937 rng(seed + 9999); // deterministic per seed

        std::set<uint32_t> currentlyDown;
        for (double t = churnStart; t < simTime - churnInterval; t += churnInterval)
        {
            // Bring previous batch back up
            std::set<uint32_t> prevDown = currentlyDown;

            // Shuffle and pick new batch
            std::shuffle(churnPool.begin(), churnPool.end(), rng);
            currentlyDown.clear();
            uint32_t toFail = std::min(churnNodes, static_cast<uint32_t>(churnPool.size()));
            for (uint32_t i = 0; i < toFail; ++i)
            {
                currentlyDown.insert(churnPool[i]);
            }

            // Schedule: bring previous down nodes back up, take new ones down
            for (uint32_t idx : prevDown)
            {
                Ptr<Ipv4> nodeIpv4 = nodes.Get(idx)->GetObject<Ipv4>();
                Simulator::Schedule(Seconds(t), &Ipv4::SetUp, nodeIpv4,
                                    static_cast<uint32_t>(1));
            }
            for (uint32_t idx : currentlyDown)
            {
                if (prevDown.count(idx) == 0) // don't re-down nodes that are already down
                {
                    Ptr<Ipv4> nodeIpv4 = nodes.Get(idx)->GetObject<Ipv4>();
                    Simulator::Schedule(Seconds(t), &Ipv4::SetDown, nodeIpv4,
                                        static_cast<uint32_t>(1));
                }
            }
            // Bring up nodes that were down but aren't in the new batch
            for (uint32_t idx : prevDown)
            {
                if (currentlyDown.count(idx) == 0)
                {
                    // Already scheduled SetUp above
                }
            }
        }

        // At sim end minus 30s, bring everyone back up for final measurements
        for (uint32_t idx : currentlyDown)
        {
            Ptr<Ipv4> nodeIpv4 = nodes.Get(idx)->GetObject<Ipv4>();
            Simulator::Schedule(Seconds(simTime - 30.0), &Ipv4::SetUp, nodeIpv4,
                                static_cast<uint32_t>(1));
        }
    }

    // -------------------------------------------------------------------------
    // Connect PHY-level TX sniffer for ALL protocols
    // -------------------------------------------------------------------------
    ConnectPhyTxTraces(wifiDevices);

    // -------------------------------------------------------------------------
    // Connect TAVRN trace sources (must be done after stack installation)
    // -------------------------------------------------------------------------
    if (protocol == "TAVRN")
    {
        ConnectTavrnTraces(nodes);
    }

    // -------------------------------------------------------------------------
    // Applications: CBR UDP flows between deterministic pairs
    // -------------------------------------------------------------------------
    uint64_t bitsPerSecond = static_cast<uint64_t>(packetSize) * 8 *
                             static_cast<uint64_t>(dataRate);
    std::ostringstream dataRateStr;
    dataRateStr << bitsPerSecond << "bps";

    uint32_t actualFlows = std::min(nFlows, nNodes / 2);

    // 60s warmup for all protocols for fairness
    double appStartBase = 60.0;

    // Set steady-state measurement start to when apps begin
    // (all protocols have had equal warmup time by this point)
    g_steadyStateStartSec = appStartBase;

    // -------------------------------------------------------------------------
    // WifiCoTraceHelper: track per-state radio durations (TX, RX, IDLE, etc.)
    // Measures steady-state window only [appStartBase, simTime] for fair
    // comparison. TX+RX energy isolates the protocol's active radio cost,
    // which is what matters on duty-cycled radios (BLE mesh target).
    // -------------------------------------------------------------------------
    WifiCoTraceHelper coHelper(Seconds(appStartBase), Seconds(simTime));
    coHelper.Enable(wifiDevices);

    ApplicationContainer sinkApps;
    ApplicationContainer sourceApps;

    for (uint32_t f = 0; f < actualFlows; ++f)
    {
        uint32_t srcIdx;
        uint32_t dstIdx;

        if (hubTraffic)
        {
            // Mixed hub traffic: first half of flows → node 0 (sensors→hub),
            // second half = peer-to-peer (switch→light, etc.)
            if (f < actualFlows / 2 + actualFlows % 2)
            {
                // Hub-bound: source is node f+1 (skip hub=0), destination is 0
                srcIdx = f + 1;
                if (srcIdx >= nNodes) srcIdx = nNodes - 1;
                dstIdx = 0;
            }
            else
            {
                // Peer-to-peer: spread across non-hub nodes
                uint32_t peerF = f - (actualFlows / 2 + actualFlows % 2);
                srcIdx = 1 + peerF * 2;
                dstIdx = 2 + peerF * 2;
                if (srcIdx >= nNodes) srcIdx = nNodes - 2;
                if (dstIdx >= nNodes) dstIdx = nNodes - 1;
            }
        }
        else
        {
            // Default: opposite-corner pairing
            srcIdx = f;
            dstIdx = nNodes - 1 - f;
        }

        if (srcIdx == dstIdx)
        {
            dstIdx = (dstIdx + 1) % nNodes;
            if (srcIdx == dstIdx)
            {
                continue;
            }
        }

        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), appPort + f));
        ApplicationContainer sinkApp = sinkHelper.Install(nodes.Get(dstIdx));
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(simTime));
        sinkApps.Add(sinkApp);

        OnOffHelper onOffHelper(
            "ns3::UdpSocketFactory",
            InetSocketAddress(interfaces.GetAddress(dstIdx), appPort + f));
        std::ostringstream onTimeStr, offTimeStr;
        onTimeStr << "ns3::ConstantRandomVariable[Constant=" << onTime << "]";
        offTimeStr << "ns3::ConstantRandomVariable[Constant=" << offTime << "]";
        onOffHelper.SetAttribute("OnTime", StringValue(onTimeStr.str()));
        onOffHelper.SetAttribute("OffTime", StringValue(offTimeStr.str()));
        onOffHelper.SetAttribute("PacketSize", UintegerValue(packetSize));
        onOffHelper.SetAttribute("DataRate", StringValue(dataRateStr.str()));

        double stagger = static_cast<double>(f) * 0.5;
        ApplicationContainer srcApp = onOffHelper.Install(nodes.Get(srcIdx));
        srcApp.Start(Seconds(appStartBase + stagger));
        srcApp.Stop(Seconds(simTime - 1.0));
        sourceApps.Add(srcApp);
    }

    // -------------------------------------------------------------------------
    // FlowMonitor: for application-level metrics (PDR, latency)
    // -------------------------------------------------------------------------
    FlowMonitorHelper flowMonHelper;
    Ptr<FlowMonitor> flowMon = flowMonHelper.InstallAll();

    // -------------------------------------------------------------------------
    // Run simulation
    // -------------------------------------------------------------------------
    NS_LOG_INFO("Starting " << protocol << " (" << scenario << ") seed=" << seed
                             << " nNodes=" << nNodes);

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // -------------------------------------------------------------------------
    // Collect results
    // -------------------------------------------------------------------------
    flowMon->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowMonHelper.GetClassifier());

    uint32_t totalTxPackets = 0;
    uint32_t totalRxPackets = 0;
    double totalDelayMs = 0.0;
    uint32_t delayCount = 0;
    uint64_t appDataTxBytes = 0;

    const auto& stats = flowMon->GetFlowStats();
    for (const auto& [flowId, flowStats] : stats)
    {
        Ipv4FlowClassifier::FiveTuple tuple = classifier->FindFlow(flowId);

        bool isAppFlow = false;
        for (uint32_t f = 0; f < actualFlows; ++f)
        {
            if (tuple.destinationPort == appPort + f)
            {
                isAppFlow = true;
                break;
            }
        }

        if (isAppFlow)
        {
            totalTxPackets += flowStats.txPackets;
            totalRxPackets += flowStats.rxPackets;
            appDataTxBytes += flowStats.txBytes;

            if (flowStats.rxPackets > 0)
            {
                totalDelayMs += flowStats.delaySum.GetMilliSeconds();
                delayCount += flowStats.rxPackets;
            }
        }
    }

    // Control overhead = total PHY TX bytes - application data TX bytes.
    // This is uniform across all protocols and captures broadcast control traffic
    // that FlowMonitor misses for AODV/OLSR.
    // Note: appDataTxBytes from FlowMonitor counts IP-level bytes (including headers),
    // while g_totalPhyTxBytes counts MAC-level bytes. The difference captures:
    //   - All routing control messages (RREQ, RREP, RERR, HELLO, TC, etc.)
    //   - MAC-level retransmissions
    //   - MAC/PHY overhead on data frames
    // This is a standard approach in MANET protocol comparison papers.
    // Total overhead (entire simulation)
    uint64_t totalOverheadBytes = 0;
    if (g_totalPhyTxBytes > appDataTxBytes)
    {
        totalOverheadBytes = g_totalPhyTxBytes - appDataTxBytes;
    }

    // Bootstrap overhead = PHY bytes during [0, appStartBase) only
    // bootstrapPhyBytes = totalPhyBytes - steadyStatePhyBytes
    uint64_t bootstrapPhyBytes = g_totalPhyTxBytes - g_steadyStatePhyTxBytes;
    uint64_t bootstrapOverheadBytes = bootstrapPhyBytes; // No app data during bootstrap

    // Steady-state control overhead rate (bytes/s)
    // = (steadyStatePhyTx - appDataTx) / steadyStateDuration
    double steadyStateDuration = simTime - appStartBase;
    double steadyStateOverheadBps = 0.0;
    if (steadyStateDuration > 0.0 && g_steadyStatePhyTxBytes > appDataTxBytes)
    {
        steadyStateOverheadBps =
            static_cast<double>(g_steadyStatePhyTxBytes - appDataTxBytes) / steadyStateDuration;
    }

    // PDR
    result.pdr = (totalTxPackets > 0)
                     ? (static_cast<double>(totalRxPackets) / totalTxPackets)
                     : 0.0;

    // Average end-to-end latency
    result.avgLatencyMs = (delayCount > 0) ? (totalDelayMs / delayCount) : 0.0;

    // Overhead metrics
    result.totalOverheadBytes = totalOverheadBytes;
    result.bootstrapOverheadBytes = bootstrapOverheadBytes;
    result.steadyStateOverheadBps = steadyStateOverheadBps;

    // Total energy consumed (includes idle — reported for convention)
    result.energyJ = 0.0;
    for (auto iter = deviceEnergyModels.Begin(); iter != deviceEnergyModels.End(); ++iter)
    {
        result.energyJ += (*iter)->GetTotalEnergyConsumption();
    }

    // TX/RX-only energy from WifiCoTraceHelper (active radio cost).
    // In a duty-cycled deployment (BLE mesh target), idle power is eliminated
    // by sleep scheduling. TX+RX energy isolates the protocol's actual
    // radio cost — the metric that differentiates protocols.
    constexpr double kSupplyVoltage = 3.3;   // matches BasicEnergySource config
    constexpr double kTxCurrentA = 0.380;    // matches WifiRadioEnergyModel config
    constexpr double kRxCurrentA = 0.313;    // matches WifiRadioEnergyModel config
    double totalTxTimeSec = 0.0;
    double totalRxTimeSec = 0.0;
    const auto& records = coHelper.GetDeviceRecords();
    for (const auto& rec : records)
    {
        for (const auto& [linkId, stateMap] : rec.m_linkStateDurations)
        {
            auto txIt = stateMap.find(WifiPhyState::TX);
            if (txIt != stateMap.end())
            {
                totalTxTimeSec += txIt->second.GetSeconds();
            }
            auto rxIt = stateMap.find(WifiPhyState::RX);
            if (rxIt != stateMap.end())
            {
                totalRxTimeSec += rxIt->second.GetSeconds();
            }
        }
    }
    result.txEnergyJ = totalTxTimeSec * kTxCurrentA * kSupplyVoltage;
    result.rxEnergyJ = totalRxTimeSec * kRxCurrentA * kSupplyVoltage;

    // Topology accuracy (TAVRN GTT or OLSR topology table)
    result.gttAccuracy = -1.0;
    if (protocol == "TAVRN")
    {
        result.gttAccuracy = ComputeAverageGttAccuracy(nodes);
    }
    else if (protocol == "OLSR")
    {
        result.gttAccuracy = ComputeOlsrTopologyAccuracy(nodes);
    }

    // Convergence time (TAVRN only)
    result.avgConvergenceMs = 0.0;
    if (!g_convergenceTimesMs.empty())
    {
        double sum = std::accumulate(g_convergenceTimesMs.begin(),
                                     g_convergenceTimesMs.end(),
                                     0.0);
        result.avgConvergenceMs = sum / g_convergenceTimesMs.size();
    }

    // Route discovery latency (TAVRN only)
    result.avgRouteDiscoveryMs = 0.0;
    if (!g_routeDiscoveryTimesMs.empty())
    {
        double sum = std::accumulate(g_routeDiscoveryTimesMs.begin(),
                                     g_routeDiscoveryTimesMs.end(),
                                     0.0);
        result.avgRouteDiscoveryMs = sum / g_routeDiscoveryTimesMs.size();
    }

    Simulator::Destroy();
    return result;
}

// =============================================================================
// Statistics helpers
// =============================================================================

static double
Mean(const std::vector<double>& v)
{
    if (v.empty())
    {
        return 0.0;
    }
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

static double
StdDev(const std::vector<double>& v, double mean)
{
    if (v.size() < 2)
    {
        return 0.0;
    }
    double sumSq = 0.0;
    for (double x : v)
    {
        sumSq += (x - mean) * (x - mean);
    }
    return std::sqrt(sumSq / (v.size() - 1));
}

/// 95% CI half-width: t_{0.025, n-1} * stddev / sqrt(n)
/// For simplicity, use z=1.96 (normal approx, valid for n >= 10)
static double
Ci95(const std::vector<double>& v, double mean)
{
    if (v.size() < 2)
    {
        return 0.0;
    }
    double sd = StdDev(v, mean);
    return 1.96 * sd / std::sqrt(static_cast<double>(v.size()));
}

// =============================================================================
// Main
// =============================================================================

int
main(int argc, char* argv[])
{
    // -------------------------------------------------------------------------
    // Command-line parameters with defaults
    // -------------------------------------------------------------------------
    std::string protocol = "TAVRN";
    uint32_t nNodes = 25;
    double simTime = 300.0;
    uint32_t nFlows = 5;
    double areaSize = 500.0;
    uint32_t seedStart = 1;
    uint32_t nRuns = 1;
    std::string scenario = "grid"; // "grid", "linear", or "mobile"
    bool verbose = false;
    uint16_t appPort = 9;
    uint32_t packetSize = 512;
    double dataRate = 4.0;    // packets per second per flow
    double maxSpeed = 1.0;    // max node speed for mobile (m/s)
    double pauseTime = 5.0;   // pause time for mobile (s)
    uint32_t failNodes = 0;   // number of nodes to fail mid-simulation
    double failTime = 300.0;  // when to fail nodes (s)
    double rejoinTime = -1.0; // when to rejoin nodes (s), negative = never
    double onTime = 1.0;      // OnOff on-duration (s)
    double offTime = 0.0;     // OnOff off-duration (s)
    uint32_t churnNodes = 0;  // number of nodes in rolling churn pool
    double churnInterval = 60.0; // seconds between churn rotations
    bool hubTraffic = false;  // mixed hub traffic: half flows → node 0, half peer-to-peer

    CommandLine cmd(__FILE__);
    cmd.AddValue("protocol", "Routing protocol: TAVRN, AODV, OLSR, DSDV", protocol);
    cmd.AddValue("nNodes", "Number of nodes", nNodes);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("nFlows", "Number of CBR UDP flows", nFlows);
    cmd.AddValue("areaSize", "Side length of square area in meters", areaSize);
    cmd.AddValue("seedStart", "Starting random seed (multi-seed sweep)", seedStart);
    cmd.AddValue("nRuns", "Number of runs for CI (set > 1 for sweep)", nRuns);
    cmd.AddValue("scenario", "Scenario: grid, linear, mobile, home", scenario);
    cmd.AddValue("verbose", "Enable verbose logging", verbose);
    cmd.AddValue("appPort", "Application UDP port", appPort);
    cmd.AddValue("packetSize", "UDP packet size in bytes", packetSize);
    cmd.AddValue("dataRate", "Packets per second per flow", dataRate);
    cmd.AddValue("maxSpeed", "Max node speed for mobile scenario (m/s)", maxSpeed);
    cmd.AddValue("pauseTime", "Pause time for mobile scenario (s)", pauseTime);
    cmd.AddValue("failNodes", "Number of nodes to fail at failTime", failNodes);
    cmd.AddValue("failTime", "Time to fail nodes (s)", failTime);
    cmd.AddValue("rejoinTime", "Time to rejoin failed nodes (s), negative=never", rejoinTime);
    cmd.AddValue("onTime", "OnOff application on-duration (s)", onTime);
    cmd.AddValue("offTime", "OnOff application off-duration (s)", offTime);
    cmd.AddValue("churnNodes", "Rolling churn: nodes cycling offline per interval", churnNodes);
    cmd.AddValue("churnInterval", "Rolling churn: seconds between rotations", churnInterval);
    cmd.AddValue("hubTraffic", "Mixed hub traffic: half flows to node 0, half peer-to-peer", hubTraffic);
    cmd.Parse(argc, argv);

    if (scenario != "grid" && scenario != "linear" && scenario != "mobile" &&
        scenario != "home")
    {
        NS_FATAL_ERROR("Unknown scenario: " << scenario
                                             << ". Use 'grid', 'linear', 'mobile', or 'home'.");
    }

    // -------------------------------------------------------------------------
    // Multi-seed sweep with aggregation
    // -------------------------------------------------------------------------
    std::vector<double> allPdr;
    std::vector<double> allLatency;
    std::vector<double> allSteadyBps;
    std::vector<double> allBootstrapBytes;
    std::vector<double> allTotalOverhead;
    std::vector<double> allEnergy;
    std::vector<double> allTxEnergy;
    std::vector<double> allRxEnergy;
    std::vector<double> allGttAccuracy;
    std::vector<double> allConvergence;
    std::vector<double> allRouteDiscovery;

    // Print CSV header
    if (nRuns == 1)
    {
        std::cout << "protocol,scenario,nNodes,nFlows,simTime,seed,"
                  << "pdr,avgLatencyMs,"
                  << "steadyStateOverheadBps,bootstrapOverheadBytes,totalOverheadBytes,"
                  << "energyJ,txEnergyJ,rxEnergyJ,"
                  << "gttAccuracy,avgConvergenceMs,avgRouteDiscoveryMs"
                  << std::endl;
    }
    else
    {
        std::cout << "protocol,scenario,nNodes,nFlows,simTime,nRuns,"
                  << "pdr_mean,pdr_ci95,"
                  << "latency_mean,latency_ci95,"
                  << "steadyBps_mean,steadyBps_ci95,"
                  << "bootstrapBytes_mean,bootstrapBytes_ci95,"
                  << "totalOverhead_mean,totalOverhead_ci95,"
                  << "energy_mean,energy_ci95,"
                  << "txEnergy_mean,txEnergy_ci95,"
                  << "rxEnergy_mean,rxEnergy_ci95,"
                  << "gttAccuracy_mean,gttAccuracy_ci95,"
                  << "convergence_mean,convergence_ci95,"
                  << "routeDiscovery_mean,routeDiscovery_ci95"
                  << std::endl;
    }

    for (uint32_t run = 0; run < nRuns; ++run)
    {
        uint32_t seed = seedStart + run;

        RunResult r = RunSingleSimulation(protocol,
                                           scenario,
                                           nNodes,
                                           simTime,
                                           nFlows,
                                           areaSize,
                                           seed,
                                           appPort,
                                           packetSize,
                                           dataRate,
                                           maxSpeed,
                                           pauseTime,
                                           failNodes,
                                           failTime,
                                           rejoinTime,
                                           onTime,
                                           offTime,
                                           churnNodes,
                                           churnInterval,
                                           hubTraffic,
                                           verbose);

        allPdr.push_back(r.pdr);
        allLatency.push_back(r.avgLatencyMs);
        allSteadyBps.push_back(r.steadyStateOverheadBps);
        allBootstrapBytes.push_back(static_cast<double>(r.bootstrapOverheadBytes));
        allTotalOverhead.push_back(static_cast<double>(r.totalOverheadBytes));
        allEnergy.push_back(r.energyJ);
        allTxEnergy.push_back(r.txEnergyJ);
        allRxEnergy.push_back(r.rxEnergyJ);
        allGttAccuracy.push_back(r.gttAccuracy);
        allConvergence.push_back(r.avgConvergenceMs);
        allRouteDiscovery.push_back(r.avgRouteDiscoveryMs);

        // For single runs, output per-run CSV line
        if (nRuns == 1)
        {
            std::cout << std::fixed << std::setprecision(2);
            std::cout << protocol << ","
                      << scenario << ","
                      << nNodes << ","
                      << nFlows << ","
                      << simTime << ","
                      << seed << ","
                      << r.pdr << ","
                      << r.avgLatencyMs << ","
                      << r.steadyStateOverheadBps << ","
                      << r.bootstrapOverheadBytes << ","
                      << r.totalOverheadBytes << ","
                      << r.energyJ << ","
                      << r.txEnergyJ << ","
                      << r.rxEnergyJ << ","
                      << r.gttAccuracy << ","
                      << r.avgConvergenceMs << ","
                      << r.avgRouteDiscoveryMs
                      << std::endl;
        }

        // Output TAVRN-specific extended metrics to stderr for each run
        if (protocol == "TAVRN")
        {
            std::cerr << "#TAVRN_RUN,"
                      << seed << ","
                      << "gtt_accuracy=" << r.gttAccuracy << ","
                      << "convergence_ms=" << r.avgConvergenceMs << ","
                      << "route_discovery_ms=" << r.avgRouteDiscoveryMs << ","
                      << "steady_bps=" << r.steadyStateOverheadBps << ","
                      << "bootstrap_bytes=" << r.bootstrapOverheadBytes << ","
                      << "tx_energy_j=" << r.txEnergyJ << ","
                      << "rx_energy_j=" << r.rxEnergyJ
                      << std::endl;
        }
    }

    // -------------------------------------------------------------------------
    // Output aggregated results with 95% CI for multi-run sweeps
    // -------------------------------------------------------------------------
    if (nRuns > 1)
    {
        double pdrMean = Mean(allPdr);
        double latMean = Mean(allLatency);
        double ssMean = Mean(allSteadyBps);
        double bsMean = Mean(allBootstrapBytes);
        double totMean = Mean(allTotalOverhead);
        double enrMean = Mean(allEnergy);
        double txeMean = Mean(allTxEnergy);
        double rxeMean = Mean(allRxEnergy);
        double gttMean = Mean(allGttAccuracy);
        double conMean = Mean(allConvergence);
        double rdMean = Mean(allRouteDiscovery);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << protocol << ","
                  << scenario << ","
                  << nNodes << ","
                  << nFlows << ","
                  << simTime << ","
                  << nRuns << ","
                  << pdrMean << "," << Ci95(allPdr, pdrMean) << ","
                  << latMean << "," << Ci95(allLatency, latMean) << ","
                  << ssMean << "," << Ci95(allSteadyBps, ssMean) << ","
                  << bsMean << "," << Ci95(allBootstrapBytes, bsMean) << ","
                  << totMean << "," << Ci95(allTotalOverhead, totMean) << ","
                  << enrMean << "," << Ci95(allEnergy, enrMean) << ","
                  << txeMean << "," << Ci95(allTxEnergy, txeMean) << ","
                  << rxeMean << "," << Ci95(allRxEnergy, rxeMean) << ","
                  << gttMean << "," << Ci95(allGttAccuracy, gttMean) << ","
                  << conMean << "," << Ci95(allConvergence, conMean) << ","
                  << rdMean << "," << Ci95(allRouteDiscovery, rdMean)
                  << std::endl;
    }

    return 0;
}

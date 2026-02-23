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
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/udp-header.h"
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

/// Total bytes received at PHY level across all nodes.
static uint64_t g_totalPhyRxBytes = 0;

/// Steady-state PHY TX bytes (after bootstrap settles).
static uint64_t g_steadyStatePhyTxBytes = 0;

/// Steady-state PHY RX bytes (after bootstrap settles).
static uint64_t g_steadyStatePhyRxBytes = 0;

/// Time after which we consider the network "steady state" (set per run).
static double g_steadyStateStartSec = 0.0;

// =============================================================================
// Trace sink callbacks
// =============================================================================

/// Port range for application flows — set per run for IP-level classification.
static uint16_t g_appPortBase = 0;
static uint32_t g_appFlowCount = 0;

/// Per-hop IP-layer bytes: ALL packets and data-only packets.
/// Both are measured at the same pipeline stage, so subtraction is valid.
static uint64_t g_totalIpTxBytes = 0;
static uint64_t g_totalIpDataBytes = 0;
static uint64_t g_steadyStateIpTxBytes = 0;
static uint64_t g_steadyStateIpDataBytes = 0;

// =============================================================================
// Temporal sampling state — periodic snapshots for time-series analysis
// =============================================================================

/// Sampling interval in seconds (0 = disabled). Set via --sampleInterval CLI.
static double g_sampleInterval = 0.0;

/// Runtime pointers for the sampling callback (set per run, cleared after).
static NodeContainer* g_nodesPtr = nullptr;
static Ptr<FlowMonitor> g_flowMonPtr = nullptr;
static Ptr<Ipv4FlowClassifier> g_classifierPtr = nullptr;
static std::string g_currentProtocol;
static double g_simTime = 0.0;

/// Previous-sample cumulative counters for delta computation.
static uint64_t g_prevSampleIpTxBytes = 0;
static uint64_t g_prevSampleIpDataBytes = 0;
static uint32_t g_prevSampleFlowTx = 0;
static uint32_t g_prevSampleFlowRx = 0;
static double g_prevSampleDelaySum = 0.0;
static uint32_t g_prevSampleDelayCount = 0;

/// A single temporal sample capturing metrics at one point in time.
struct TimeSample
{
    double timeSec;         ///< Simulation time of this sample
    double overheadBps;     ///< Window overhead rate (bytes/s)
    double pdr;             ///< Window PDR (-1 if no packets in window)
    double latencyMs;       ///< Window average latency (-1 if no packets)
    double precision;       ///< Topology precision: correct / known (-1 if N/A)
    double recall;          ///< Topology recall: correct / (alive-1) (-1 if N/A)
    double staleRatio;      ///< Stale entries / known entries (-1 if N/A)
    uint32_t aliveNodes;    ///< Ground truth: nodes with interface up
    uint32_t knownNodes;    ///< Protocol's view: nodes it thinks exist
    uint32_t staleNodes;    ///< Known but actually dead/down
};

/// Collected samples for the current run.
static std::vector<TimeSample> g_timeSamples;

/**
 * @brief IP-level TX trace — counts per-hop data bytes by port inspection.
 *
 * Connected to Ipv4L3Protocol::Tx on every node.  Fires for every IP
 * packet transmitted, including forwarded packets at intermediate hops.
 * We peek at the IP+UDP headers to classify packets by destination port.
 *
 * Data bytes counted here represent the true per-hop application load.
 * overhead = PhyTxBytes - IpDataBytes  correctly excludes multi-hop
 * data forwarding from the overhead metric.
 */
static void
Ipv4TxTraceSink(Ptr<const Packet> packet, Ptr<Ipv4> ipv4, uint32_t iface)
{
    uint32_t sz = packet->GetSize();
    bool isSteady = Simulator::Now().GetSeconds() >= g_steadyStateStartSec;

    // Count ALL IP-level TX bytes
    g_totalIpTxBytes += sz;
    if (isSteady)
    {
        g_steadyStateIpTxBytes += sz;
    }

    // Classify: peek at IP header to check if it's UDP app data
    Ipv4Header ipHeader;
    Ptr<Packet> copy = packet->Copy();
    copy->RemoveHeader(ipHeader);

    if (ipHeader.GetProtocol() == 17) // UDP
    {
        UdpHeader udpHeader;
        copy->PeekHeader(udpHeader);
        uint16_t dstPort = udpHeader.GetDestinationPort();

        if (dstPort >= g_appPortBase && dstPort < g_appPortBase + g_appFlowCount)
        {
            g_totalIpDataBytes += sz;
            if (isSteady)
            {
                g_steadyStateIpDataBytes += sz;
            }
        }
    }
}

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
 * For overhead computation, data bytes are tracked separately at the
 * IP layer via Ipv4TxTraceSink (which correctly counts forwarded copies).
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

/**
 * @brief PHY-level RX sniffer — counts ALL received bytes.
 *
 * Connected to MonitorSnifferRx on every node's WifiPhy.
 * Mirrors the TX sniffer for symmetric measurement.
 */
static void
PhyRxSnifferSink(Ptr<const Packet> packet,
                 uint16_t channelFreq,
                 WifiTxVector txVector,
                 MpduInfo aMpdu,
                 SignalNoiseDbm signalNoise,
                 uint16_t staId)
{
    uint32_t sz = packet->GetSize();
    g_totalPhyRxBytes += sz;
    if (Simulator::Now().GetSeconds() >= g_steadyStateStartSec)
    {
        g_steadyStatePhyRxBytes += sz;
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
// Connect IP-level TX trace on all nodes for per-hop data classification
// =============================================================================

/**
 * @brief Connect Ipv4L3Protocol::Tx on every node.
 *
 * Fires for every IP packet transmitted (including forwarded).  The callback
 * inspects UDP destination port to classify data vs control packets.
 */
static void
ConnectIpTxTraces(NodeContainer& nodes)
{
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4L3Protocol> ipv4l3 =
            nodes.Get(i)->GetObject<Ipv4L3Protocol>();
        if (ipv4l3)
        {
            ipv4l3->TraceConnectWithoutContext("Tx",
                                               MakeCallback(&Ipv4TxTraceSink));
        }
    }
}

// =============================================================================
// Connect PHY-level TX/RX sniffers on all nodes (protocol-agnostic)
// =============================================================================

/**
 * @brief Connect MonitorSnifferTx and MonitorSnifferRx on every node's
 *        WifiPhy for symmetric TX/RX byte measurement.
 */
static void
ConnectPhyTraces(NetDeviceContainer& wifiDevices)
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
            phy->TraceConnectWithoutContext("MonitorSnifferRx",
                                           MakeCallback(&PhyRxSnifferSink));
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
// Topology precision / recall / stale ratio — unified for TAVRN + OLSR
// =============================================================================

/// Result of a topology accuracy snapshot.
struct TopologyMetrics
{
    double precision;    ///< correct / known (-1 if N/A)
    double recall;       ///< correct / (alive-1) (-1 if N/A)
    double staleRatio;   ///< stale / known (-1 if N/A)
    uint32_t aliveCount;
    uint32_t knownCount; ///< average across alive nodes
    uint32_t staleCount; ///< average across alive nodes
};

/**
 * @brief Helper to extract the specific routing protocol instance from a node.
 *
 * Handles the common case where Ipv4ListRouting wraps the actual protocol.
 */
template <typename T>
static Ptr<T>
GetRoutingProtocolFromNode(Ptr<Node> node)
{
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    if (!ipv4)
    {
        return nullptr;
    }
    Ptr<Ipv4RoutingProtocol> proto = ipv4->GetRoutingProtocol();
    if (!proto)
    {
        return nullptr;
    }
    Ptr<T> result = DynamicCast<T>(proto);
    if (result)
    {
        return result;
    }
    Ptr<Ipv4ListRouting> listRouting = DynamicCast<Ipv4ListRouting>(proto);
    if (listRouting)
    {
        for (uint32_t j = 0; j < listRouting->GetNRoutingProtocols(); ++j)
        {
            int16_t priority;
            Ptr<Ipv4RoutingProtocol> rp = listRouting->GetRoutingProtocol(j, priority);
            result = DynamicCast<T>(rp);
            if (result)
            {
                return result;
            }
        }
    }
    return nullptr;
}

/**
 * @brief Compute topology precision, recall, and stale ratio for any protocol.
 *
 * Precision = |known ∩ alive| / |known|     (how much of what we know is correct)
 * Recall    = |known ∩ alive| / (|alive|-1)  (how many alive nodes we know about)
 * StaleRatio = |known - alive| / |known|     (fraction of stale entries)
 *
 * For TAVRN: "known" = non-departed GTT entries.
 * For OLSR/OLSR-Tuned: "known" = union of NeighborSet + TwoHopNeighborSet + TopologySet.
 * For AODV/DSDV: returns -1 (no accessible topology table).
 */
static TopologyMetrics
ComputeTopologyMetrics(NodeContainer& nodes, const std::string& protocol)
{
    TopologyMetrics result{-1.0, -1.0, -1.0, 0, 0, 0};

    if (protocol != "TAVRN" && protocol != "OLSR" && protocol != "OLSR-Tuned")
    {
        return result; // No topology introspection for AODV/DSDV
    }

    // Step 1: Ground truth — alive nodes (interface up)
    std::set<Ipv4Address> aliveAddrs;
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
        if (ipv4 && ipv4->IsUp(1))
        {
            aliveAddrs.insert(ipv4->GetAddress(1, 0).GetLocal());
        }
    }

    if (aliveAddrs.size() <= 1)
    {
        result.aliveCount = aliveAddrs.size();
        result.precision = 1.0;
        result.recall = 1.0;
        result.staleRatio = 0.0;
        return result;
    }

    result.aliveCount = aliveAddrs.size();

    double sumPrecision = 0.0;
    double sumRecall = 0.0;
    double sumStale = 0.0;
    uint32_t count = 0;
    uint32_t totalKnown = 0;
    uint32_t totalStale = 0;

    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
        if (!ipv4 || !ipv4->IsUp(1))
        {
            continue;
        }
        Ipv4Address myAddr = ipv4->GetAddress(1, 0).GetLocal();

        // Collect this node's view of the topology
        std::set<Ipv4Address> knownAddrs;

        if (protocol == "TAVRN")
        {
            Ptr<tavrn::RoutingProtocol> tavrnProto =
                GetRoutingProtocolFromNode<tavrn::RoutingProtocol>(nodes.Get(i));
            if (tavrnProto)
            {
                for (const auto& addr : tavrnProto->GetGtt().EnumerateNodes())
                {
                    if (addr != myAddr)
                    {
                        knownAddrs.insert(addr);
                    }
                }
            }
        }
        else // OLSR or OLSR-Tuned
        {
            Ptr<olsr::RoutingProtocol> olsrProto =
                GetRoutingProtocolFromNode<olsr::RoutingProtocol>(nodes.Get(i));
            if (olsrProto)
            {
                const olsr::OlsrState& state = olsrProto->GetOlsrState();
                for (const auto& nb : state.GetNeighbors())
                {
                    knownAddrs.insert(nb.neighborMainAddr);
                }
                for (const auto& th : state.GetTwoHopNeighbors())
                {
                    knownAddrs.insert(th.neighborMainAddr);
                    knownAddrs.insert(th.twoHopNeighborAddr);
                }
                for (const auto& tc : state.GetTopologySet())
                {
                    knownAddrs.insert(tc.destAddr);
                    knownAddrs.insert(tc.lastAddr);
                }
                knownAddrs.erase(myAddr);
            }
        }

        if (knownAddrs.empty())
        {
            continue;
        }

        uint32_t correct = 0;
        uint32_t stale = 0;
        for (const auto& addr : knownAddrs)
        {
            if (aliveAddrs.count(addr) > 0)
            {
                ++correct;
            }
            else
            {
                ++stale;
            }
        }

        double nodePrecision =
            static_cast<double>(correct) / static_cast<double>(knownAddrs.size());
        double nodeRecall =
            static_cast<double>(correct) / static_cast<double>(aliveAddrs.size() - 1);
        double nodeStale =
            static_cast<double>(stale) / static_cast<double>(knownAddrs.size());

        sumPrecision += nodePrecision;
        sumRecall += nodeRecall;
        sumStale += nodeStale;
        totalKnown += knownAddrs.size();
        totalStale += stale;
        ++count;
    }

    if (count > 0)
    {
        result.precision = sumPrecision / count;
        result.recall = sumRecall / count;
        result.staleRatio = sumStale / count;
        result.knownCount = totalKnown / count;
        result.staleCount = totalStale / count;
    }

    return result;
}

// =============================================================================
// Periodic temporal sampling callback
// =============================================================================

/**
 * @brief Scheduled callback that captures a snapshot of all metrics.
 *
 * Called every g_sampleInterval seconds during the simulation.
 * Computes deltas from last sample for windowed metrics, and point-in-time
 * topology accuracy metrics.
 */
static void
DoTemporalSample()
{
    TimeSample s{};
    s.timeSec = Simulator::Now().GetSeconds();

    // --- Window overhead (IP-layer) ---
    uint64_t curIpTx = g_totalIpTxBytes;
    uint64_t curIpData = g_totalIpDataBytes;
    uint64_t deltaIpTx = curIpTx - g_prevSampleIpTxBytes;
    uint64_t deltaIpData = curIpData - g_prevSampleIpDataBytes;
    s.overheadBps = (deltaIpTx > deltaIpData)
                        ? static_cast<double>(deltaIpTx - deltaIpData) / g_sampleInterval
                        : 0.0;
    g_prevSampleIpTxBytes = curIpTx;
    g_prevSampleIpDataBytes = curIpData;

    // --- Window PDR and latency from FlowMonitor ---
    s.pdr = -1.0;
    s.latencyMs = -1.0;
    if (g_flowMonPtr && g_classifierPtr)
    {
        uint32_t curFlowTx = 0;
        uint32_t curFlowRx = 0;
        double curDelaySum = 0.0;
        uint32_t curDelayCount = 0;

        const auto& stats = g_flowMonPtr->GetFlowStats();
        for (const auto& [flowId, fs] : stats)
        {
            Ipv4FlowClassifier::FiveTuple tuple = g_classifierPtr->FindFlow(flowId);
            bool isApp = (tuple.destinationPort >= g_appPortBase &&
                          tuple.destinationPort < g_appPortBase + g_appFlowCount);
            if (isApp)
            {
                curFlowTx += fs.txPackets;
                curFlowRx += fs.rxPackets;
                if (fs.rxPackets > 0)
                {
                    curDelaySum += fs.delaySum.GetMilliSeconds();
                    curDelayCount += fs.rxPackets;
                }
            }
        }

        uint32_t deltaTx = curFlowTx - g_prevSampleFlowTx;
        uint32_t deltaRx = curFlowRx - g_prevSampleFlowRx;
        if (deltaTx > 0)
        {
            s.pdr = static_cast<double>(deltaRx) / static_cast<double>(deltaTx);
        }

        double deltaDelay = curDelaySum - g_prevSampleDelaySum;
        uint32_t deltaDelayN = curDelayCount - g_prevSampleDelayCount;
        if (deltaDelayN > 0)
        {
            s.latencyMs = deltaDelay / deltaDelayN;
        }

        g_prevSampleFlowTx = curFlowTx;
        g_prevSampleFlowRx = curFlowRx;
        g_prevSampleDelaySum = curDelaySum;
        g_prevSampleDelayCount = curDelayCount;
    }

    // --- Topology precision / recall / stale ---
    if (g_nodesPtr)
    {
        TopologyMetrics tm = ComputeTopologyMetrics(*g_nodesPtr, g_currentProtocol);
        s.precision = tm.precision;
        s.recall = tm.recall;
        s.staleRatio = tm.staleRatio;
        s.aliveNodes = tm.aliveCount;
        s.knownNodes = tm.knownCount;
        s.staleNodes = tm.staleCount;
    }

    g_timeSamples.push_back(s);

    // Reschedule next sample (stop before sim end to avoid edge effects)
    if (Simulator::Now().GetSeconds() + g_sampleInterval < g_simTime - 1.0)
    {
        Simulator::Schedule(Seconds(g_sampleInterval), &DoTemporalSample);
    }
}

// =============================================================================
// Per-run result structure
// =============================================================================

struct RunResult
{
    double pdr;
    double avgLatencyMs;
    uint64_t steadyTxBytes;            // Raw PHY TX bytes during steady state
    uint64_t steadyRxBytes;            // Raw PHY RX bytes during steady state
    uint64_t totalOverheadBytes;       // PhyTx - IpDataTx over full sim (true overhead)
    uint64_t bootstrapOverheadBytes;   // PHY bytes during bootstrap phase only
    double steadyOverheadBps;          // (steadyPhyTx - steadyIpData) / duration
    double energyJ;                    // Total energy (incl. idle) — for convention
    double txEnergyJ;                  // TX-only energy (active radio cost)
    double rxEnergyJ;                  // RX-only energy (active radio cost)
    double gttAccuracy; // -1.0 for non-TAVRN
    double avgConvergenceMs;
    double avgRouteDiscoveryMs;

    // Temporal metrics (from periodic sampling)
    double twPrecision;       ///< Time-weighted avg topology precision (-1 if N/A)
    double twRecall;          ///< Time-weighted avg topology recall (-1 if N/A)
    double avgStaleRatio;     ///< Time-weighted avg stale route ratio (-1 if N/A)
    double firstDeliveryMs;   ///< Time from first TX to first RX across all flows (-1 if none)
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
    g_totalPhyRxBytes = 0;
    g_steadyStatePhyTxBytes = 0;
    g_steadyStatePhyRxBytes = 0;
    g_totalIpTxBytes = 0;
    g_totalIpDataBytes = 0;
    g_steadyStateIpTxBytes = 0;
    g_steadyStateIpDataBytes = 0;
    g_steadyStateStartSec = 0.0; // Will be set below after appStartBase is defined
    g_appPortBase = appPort;
    g_appFlowCount = std::min(nFlows, nNodes / 2);

    // Reset temporal sampling state
    g_timeSamples.clear();
    g_prevSampleIpTxBytes = 0;
    g_prevSampleIpDataBytes = 0;
    g_prevSampleFlowTx = 0;
    g_prevSampleFlowRx = 0;
    g_prevSampleDelaySum = 0.0;
    g_prevSampleDelayCount = 0;
    g_nodesPtr = nullptr;
    g_flowMonPtr = nullptr;
    g_classifierPtr = nullptr;
    g_currentProtocol = protocol;
    g_simTime = simTime;

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
        tavrn.Set("SoftExpiryThreshold", DoubleValue(0.5));
        tavrn.Set("EnablePeriodicHello", BooleanValue(true));
        tavrn.Set("MaxMetadataEntries", UintegerValue(4));
        // Adaptive HELLO: starts at 24s (fast bootstrap), grows via EMA to 120s.
        // GttTtl stays static at 300s. Neighbor liveness checks detect silent failures.
        stack.SetRoutingHelper(tavrn);
    }
    else if (protocol == "AODV")
    {
        AodvHelper aodv;
        stack.SetRoutingHelper(aodv);
    }
    else if (protocol == "AODV-Tuned")
    {
        // Control experiment: AODV with TAVRN-like high timer values.
        // Proves that GTT enables high timers — without topology awareness,
        // AODV cannot detect failures or maintain routes with such long intervals.
        AodvHelper aodv;
        aodv.Set("HelloInterval", TimeValue(Seconds(150)));
        aodv.Set("ActiveRouteTimeout", TimeValue(Seconds(600)));
        aodv.Set("AllowedHelloLoss", UintegerValue(2));
        stack.SetRoutingHelper(aodv);
    }
    else if (protocol == "OLSR")
    {
        OlsrHelper olsr;
        stack.SetRoutingHelper(olsr);
    }
    else if (protocol == "OLSR-Tuned")
    {
        // Control experiment: OLSR with relaxed timer values to reduce overhead.
        // Shows the tradeoff between overhead reduction and topology accuracy
        // when a proactive protocol lengthens its broadcast intervals.
        OlsrHelper olsr;
        olsr.Set("HelloInterval", TimeValue(Seconds(30)));
        olsr.Set("TcInterval", TimeValue(Seconds(60)));
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
                                             << ". Use TAVRN, AODV, AODV-Tuned, OLSR, OLSR-Tuned, or DSDV.");
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
    ConnectPhyTraces(wifiDevices);

    // -------------------------------------------------------------------------
    // Connect TAVRN trace sources (must be done after stack installation)
    // -------------------------------------------------------------------------
    if (protocol == "TAVRN")
    {
        ConnectTavrnTraces(nodes);
    }

    // -------------------------------------------------------------------------
    // Connect IP-level TX trace for per-hop data byte classification
    // -------------------------------------------------------------------------
    ConnectIpTxTraces(nodes);

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

    // Get classifier now (available immediately after InstallAll) for sampling
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowMonHelper.GetClassifier());

    // -------------------------------------------------------------------------
    // Schedule temporal sampling (if enabled)
    // -------------------------------------------------------------------------
    if (g_sampleInterval > 0.0)
    {
        g_nodesPtr = &nodes;
        g_flowMonPtr = flowMon;
        g_classifierPtr = classifier;
        // Start sampling at the first sample interval (captures bootstrap phase too)
        Simulator::Schedule(Seconds(g_sampleInterval), &DoTemporalSample);
    }

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
    // classifier was already obtained before Simulator::Run()

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

    // ---- Overhead computation (IP-layer, per-hop correct) ----
    // Both total and data bytes are measured at the IP layer (Ipv4L3Protocol::Tx),
    // so subtraction is valid — no pipeline timing mismatch between layers.
    //
    // overhead = IpTxBytes - IpDataBytes captures:
    //   - All routing control messages (RREQ, RREP, RERR, HELLO, TC, etc.)
    //   - ARP requests/replies (go through IP)
    // This does NOT include pure MAC-level overhead (beacons, ACKs, retries),
    // which is symmetric across all protocols and irrelevant for comparison.

    // Total overhead (entire simulation)
    uint64_t totalOverheadBytes = 0;
    if (g_totalIpTxBytes > g_totalIpDataBytes)
    {
        totalOverheadBytes = g_totalIpTxBytes - g_totalIpDataBytes;
    }

    // Bootstrap overhead = IP bytes during [0, appStartBase) only
    uint64_t bootstrapIpBytes = g_totalIpTxBytes - g_steadyStateIpTxBytes;
    uint64_t bootstrapOverheadBytes = bootstrapIpBytes; // No app data during bootstrap

    // Steady-state overhead rate (bytes/s)
    double steadyStateDuration = simTime - appStartBase;
    double steadyOverheadBps = 0.0;
    if (steadyStateDuration > 0.0 && g_steadyStateIpTxBytes > g_steadyStateIpDataBytes)
    {
        steadyOverheadBps =
            static_cast<double>(g_steadyStateIpTxBytes - g_steadyStateIpDataBytes) /
            steadyStateDuration;
    }

    // PDR
    result.pdr = (totalTxPackets > 0)
                     ? (static_cast<double>(totalRxPackets) / totalTxPackets)
                     : 0.0;

    // Average end-to-end latency
    result.avgLatencyMs = (delayCount > 0) ? (totalDelayMs / delayCount) : 0.0;

    // Overhead metrics
    result.steadyTxBytes = g_steadyStatePhyTxBytes;
    result.steadyRxBytes = g_steadyStatePhyRxBytes;
    result.totalOverheadBytes = totalOverheadBytes;
    result.bootstrapOverheadBytes = bootstrapOverheadBytes;
    result.steadyOverheadBps = steadyOverheadBps;

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
    else if (protocol == "OLSR" || protocol == "OLSR-Tuned")
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

    // ---- First packet delivery time (convergence metric for ALL protocols) ----
    result.firstDeliveryMs = -1.0;
    {
        Time earliest = Seconds(simTime);
        Time earliestTx = Seconds(simTime);
        bool found = false;
        const auto& allStats = flowMon->GetFlowStats();
        for (const auto& [flowId, fs] : allStats)
        {
            Ipv4FlowClassifier::FiveTuple tuple = classifier->FindFlow(flowId);
            bool isApp = (tuple.destinationPort >= g_appPortBase &&
                          tuple.destinationPort < g_appPortBase + g_appFlowCount);
            if (isApp && fs.rxPackets > 0)
            {
                if (!found || fs.timeFirstRxPacket < earliest)
                {
                    earliest = fs.timeFirstRxPacket;
                    earliestTx = fs.timeFirstTxPacket;
                    found = true;
                }
            }
        }
        if (found)
        {
            result.firstDeliveryMs =
                (earliest.GetSeconds() - earliestTx.GetSeconds()) * 1000.0;
        }
    }

    // ---- Time-series summary metrics ----
    result.twPrecision = -1.0;
    result.twRecall = -1.0;
    result.avgStaleRatio = -1.0;
    if (!g_timeSamples.empty())
    {
        double sumPrec = 0.0;
        double sumRec = 0.0;
        double sumStale = 0.0;
        uint32_t nPrec = 0;

        for (const auto& s : g_timeSamples)
        {
            if (s.precision >= 0.0)
            {
                sumPrec += s.precision;
                sumRec += s.recall;
                sumStale += s.staleRatio;
                ++nPrec;
            }
        }
        if (nPrec > 0)
        {
            result.twPrecision = sumPrec / nPrec;
            result.twRecall = sumRec / nPrec;
            result.avgStaleRatio = sumStale / nPrec;
        }
    }

    // Clear sampling global pointers before Destroy (avoid dangling)
    g_nodesPtr = nullptr;
    g_flowMonPtr = nullptr;
    g_classifierPtr = nullptr;

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
    double sampleInterval = 0.0; // temporal sampling interval (s), 0 = disabled

    CommandLine cmd(__FILE__);
    cmd.AddValue("protocol", "Routing protocol: TAVRN, AODV, AODV-Tuned, OLSR, OLSR-Tuned, DSDV", protocol);
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
    cmd.AddValue("sampleInterval", "Temporal sampling interval in seconds (0=disabled)", sampleInterval);
    cmd.Parse(argc, argv);

    // Set global sampling interval
    g_sampleInterval = sampleInterval;

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
    std::vector<double> allSteadyTx;
    std::vector<double> allSteadyRx;
    std::vector<double> allSteadyOverheadBps;
    std::vector<double> allBootstrapBytes;
    std::vector<double> allTotalOverhead;
    std::vector<double> allEnergy;
    std::vector<double> allTxEnergy;
    std::vector<double> allRxEnergy;
    std::vector<double> allGttAccuracy;
    std::vector<double> allConvergence;
    std::vector<double> allRouteDiscovery;
    std::vector<double> allTwPrecision;
    std::vector<double> allTwRecall;
    std::vector<double> allAvgStaleRatio;
    std::vector<double> allFirstDelivery;

    // Print CSV header
    if (nRuns == 1)
    {
        std::cout << "protocol,scenario,nNodes,nFlows,simTime,seed,"
                  << "pdr,avgLatencyMs,"
                  << "steadyTxBytes,steadyRxBytes,"
                  << "steadyOverheadBps,bootstrapOverheadBytes,totalOverheadBytes,"
                  << "energyJ,txEnergyJ,rxEnergyJ,"
                  << "gttAccuracy,avgConvergenceMs,avgRouteDiscoveryMs,"
                  << "twPrecision,twRecall,avgStaleRatio,firstDeliveryMs"
                  << std::endl;
    }
    else
    {
        std::cout << "protocol,scenario,nNodes,nFlows,simTime,nRuns,"
                  << "pdr_mean,pdr_ci95,"
                  << "latency_mean,latency_ci95,"
                  << "steadyTx_mean,steadyTx_ci95,"
                  << "steadyRx_mean,steadyRx_ci95,"
                  << "steadyOverheadBps_mean,steadyOverheadBps_ci95,"
                  << "bootstrapBytes_mean,bootstrapBytes_ci95,"
                  << "totalOverhead_mean,totalOverhead_ci95,"
                  << "energy_mean,energy_ci95,"
                  << "txEnergy_mean,txEnergy_ci95,"
                  << "rxEnergy_mean,rxEnergy_ci95,"
                  << "gttAccuracy_mean,gttAccuracy_ci95,"
                  << "convergence_mean,convergence_ci95,"
                  << "routeDiscovery_mean,routeDiscovery_ci95,"
                  << "twPrecision_mean,twPrecision_ci95,"
                  << "twRecall_mean,twRecall_ci95,"
                  << "avgStaleRatio_mean,avgStaleRatio_ci95,"
                  << "firstDelivery_mean,firstDelivery_ci95"
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
        allSteadyTx.push_back(static_cast<double>(r.steadyTxBytes));
        allSteadyRx.push_back(static_cast<double>(r.steadyRxBytes));
        allSteadyOverheadBps.push_back(r.steadyOverheadBps);
        allBootstrapBytes.push_back(static_cast<double>(r.bootstrapOverheadBytes));
        allTotalOverhead.push_back(static_cast<double>(r.totalOverheadBytes));
        allEnergy.push_back(r.energyJ);
        allTxEnergy.push_back(r.txEnergyJ);
        allRxEnergy.push_back(r.rxEnergyJ);
        allGttAccuracy.push_back(r.gttAccuracy);
        allConvergence.push_back(r.avgConvergenceMs);
        allRouteDiscovery.push_back(r.avgRouteDiscoveryMs);
        allTwPrecision.push_back(r.twPrecision);
        allTwRecall.push_back(r.twRecall);
        allAvgStaleRatio.push_back(r.avgStaleRatio);
        allFirstDelivery.push_back(r.firstDeliveryMs);

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
                      << r.steadyTxBytes << ","
                      << r.steadyRxBytes << ","
                      << r.steadyOverheadBps << ","
                      << r.bootstrapOverheadBytes << ","
                      << r.totalOverheadBytes << ","
                      << r.energyJ << ","
                      << r.txEnergyJ << ","
                      << r.rxEnergyJ << ","
                      << r.gttAccuracy << ","
                      << r.avgConvergenceMs << ","
                      << r.avgRouteDiscoveryMs << ","
                      << r.twPrecision << ","
                      << r.twRecall << ","
                      << r.avgStaleRatio << ","
                      << r.firstDeliveryMs
                      << std::endl;
        }

        // Output extended per-run metrics to stderr
        std::cerr << "#RUN," << protocol << ","
                  << seed << ","
                  << "gtt_accuracy=" << r.gttAccuracy << ","
                  << "convergence_ms=" << r.avgConvergenceMs << ","
                  << "route_discovery_ms=" << r.avgRouteDiscoveryMs << ","
                  << "steady_overhead_bps=" << r.steadyOverheadBps << ","
                  << "steady_tx=" << r.steadyTxBytes << ","
                  << "steady_rx=" << r.steadyRxBytes << ","
                  << "bootstrap_bytes=" << r.bootstrapOverheadBytes << ","
                  << "tx_energy_j=" << r.txEnergyJ << ","
                  << "rx_energy_j=" << r.rxEnergyJ << ","
                  << "tw_precision=" << r.twPrecision << ","
                  << "tw_recall=" << r.twRecall << ","
                  << "avg_stale_ratio=" << r.avgStaleRatio << ","
                  << "first_delivery_ms=" << r.firstDeliveryMs
                  << std::endl;

        // Output time-series data to stderr (if sampling was enabled)
        if (!g_timeSamples.empty())
        {
            for (const auto& ts : g_timeSamples)
            {
                std::cerr << std::fixed << std::setprecision(4)
                          << "#TS," << protocol << "," << seed << ","
                          << ts.timeSec << ","
                          << ts.overheadBps << ","
                          << ts.pdr << ","
                          << ts.latencyMs << ","
                          << ts.precision << ","
                          << ts.recall << ","
                          << ts.staleRatio << ","
                          << ts.aliveNodes << ","
                          << ts.knownNodes << ","
                          << ts.staleNodes
                          << std::endl;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Output aggregated results with 95% CI for multi-run sweeps
    // -------------------------------------------------------------------------
    if (nRuns > 1)
    {
        double pdrMean = Mean(allPdr);
        double latMean = Mean(allLatency);
        double stxMean = Mean(allSteadyTx);
        double srxMean = Mean(allSteadyRx);
        double sohMean = Mean(allSteadyOverheadBps);
        double bsMean = Mean(allBootstrapBytes);
        double totMean = Mean(allTotalOverhead);
        double enrMean = Mean(allEnergy);
        double txeMean = Mean(allTxEnergy);
        double rxeMean = Mean(allRxEnergy);
        double gttMean = Mean(allGttAccuracy);
        double conMean = Mean(allConvergence);
        double rdMean = Mean(allRouteDiscovery);
        double twpMean = Mean(allTwPrecision);
        double twrMean = Mean(allTwRecall);
        double slMean = Mean(allAvgStaleRatio);
        double fdMean = Mean(allFirstDelivery);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << protocol << ","
                  << scenario << ","
                  << nNodes << ","
                  << nFlows << ","
                  << simTime << ","
                  << nRuns << ","
                  << pdrMean << "," << Ci95(allPdr, pdrMean) << ","
                  << latMean << "," << Ci95(allLatency, latMean) << ","
                  << stxMean << "," << Ci95(allSteadyTx, stxMean) << ","
                  << srxMean << "," << Ci95(allSteadyRx, srxMean) << ","
                  << sohMean << "," << Ci95(allSteadyOverheadBps, sohMean) << ","
                  << bsMean << "," << Ci95(allBootstrapBytes, bsMean) << ","
                  << totMean << "," << Ci95(allTotalOverhead, totMean) << ","
                  << enrMean << "," << Ci95(allEnergy, enrMean) << ","
                  << txeMean << "," << Ci95(allTxEnergy, txeMean) << ","
                  << rxeMean << "," << Ci95(allRxEnergy, rxeMean) << ","
                  << gttMean << "," << Ci95(allGttAccuracy, gttMean) << ","
                  << conMean << "," << Ci95(allConvergence, conMean) << ","
                  << rdMean << "," << Ci95(allRouteDiscovery, rdMean) << ","
                  << twpMean << "," << Ci95(allTwPrecision, twpMean) << ","
                  << twrMean << "," << Ci95(allTwRecall, twrMean) << ","
                  << slMean << "," << Ci95(allAvgStaleRatio, slMean) << ","
                  << fdMean << "," << Ci95(allFirstDelivery, fdMean)
                  << std::endl;
    }

    return 0;
}

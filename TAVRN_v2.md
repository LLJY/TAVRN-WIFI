# TAVRN v2.2: Topology Aware Vicinity-Reactive Network

## Overview

TAVRN is a lightweight mesh network protocol providing both reactive routing and topology awareness for service discovery. It sits directly below the application layer, comparable to Thread or ZigBee's network layer.

**Core insight**: Service discovery layers (Matter, ZigBee, CoAP) need to know *who exists* on the network. Proactive protocols (OLSR) provide this but waste energy maintaining full routing tables. Pure reactive protocols (AODV) are efficient but cannot enumerate nodes. TAVRN provides topology awareness with reactive routing efficiency by treating topology as *local knowledge* rather than *network state*.

**Design philosophy**: In constrained wireless networks, radio transmission is the dominant cost. TAVRN trades abundant local resources (memory, CPU) for network silence. Inspired by how social animals maintain relationship awareness without constant communication, TAVRN stores topology state locally and refreshes it opportunistically through piggybacked metadata, achieving near-zero standalone control traffic while providing full topology visibility.

> *Relationships are maintained in the mind, not on the wire.*

---

## Resource Economics

| Resource | Cost Trend | TAVRN Strategy |
|----------|------------|----------------|
| RAM | Decreasing yearly | Spend it (GTT storage) |
| CPU | Decreasing yearly | Spend it (local computation) |
| Radio TX | Fixed by physics | Hoard it |

Radio transmission costs are irreducible:
- Energy (mA per byte)
- Spectrum (collision probability)
- Latency (CSMA/CA backoff)
- Neighbor disruption (all nodes must process)

TAVRN explicitly trades memory and CPU cycles for radio silence.

---

## Protocol Positioning

TAVRN occupies the underserved gap between pure reactive and proactive protocols:

```
Reactive <---------------------------------------------> Proactive
   0.0        0.2              0.5              1.0
    |          |                |                |
   AODV     TAVRN <---------> OLSR <---------> |
          (tunable 0.2-0.5)  (tunable 0.5-1.0)
```

| Protocol | Range | Optimized For |
|----------|-------|---------------|
| AODV | 0.0 | Minimal overhead, no topology awareness |
| TAVRN | 0.2 -- 0.5 | Topology awareness with reactive efficiency |
| OLSR | 0.5 -- 1.0 | Full link-state, guaranteed freshness |

OLSR *can* be tuned toward reactive behavior, but its architecture assumes proactivity. Tuning it down fights its design. TAVRN is *architecturally* optimized for the lower end of the spectrum.

---

## Design Assumptions

TAVRN is explicitly designed for:

| Assumption | Rationale |
|------------|-----------|
| Relatively static MANET | Smart home, industrial IoT, building automation. Not vehicular or disaster response. |
| Medium-sized networks | GTT requires memory proportional to node count. Scales to hundreds, not thousands. |
| Stable topology | Nodes may join and leave, but the network is not highly volatile. |

These are **scoping constraints**, not weaknesses. A protocol optimized for all scenarios is optimal for none.

---

## Architecture

```
+-----------------------------------------------------------+
|  Application / Service Discovery (Matter, CoAP, etc.)     |
+-----------------------------------------------------------+
|  TAVRN v2.1                                               |
|  +- GTT (Global Topology Table) - who exists              |
|  |    +- Local knowledge, opportunistically refreshed     |
|  +- Routing (AODV-based) - how to reach                   |
|       +- Reactive discovery with next-hop caching         |
|       +- GTT-assisted Smart TTL for route discovery       |
+-----------------------------------------------------------+
|  Physical Layer                                           |
+-----------------------------------------------------------+
```

**Key architectural separation**: The GTT is a passive data structure. It never schedules timers or sends packets. All scheduling and transmission decisions are made by the RoutingProtocol that owns it. This keeps the GTT testable and deterministic.

---

## Design Principles

**Principle 1: Keep quiet unless you must transmit.**
The network aims to be as silent as reasonably possible. Standalone topology control packets are a last resort.

**Principle 2: Topology is local knowledge, not network state.**
Each node maintains its own understanding of the network. Synchronization happens opportunistically, not continuously.

**Principle 3: Piggyback topology control on existing traffic (conditionally).**
When the topology is stable, data packets are forwarded clean (AODV-sized). When GTT entries approach soft expiry, topology metadata is piggybacked on existing traffic. This ensures zero overhead when not needed, and free maintenance when needed.

---

## Social Model Mapping

TAVRN mechanisms mirror how humans and social animals maintain relationships:

| Social Behavior | TAVRN Mechanism |
|-----------------|-----------------|
| You remember who your friends are | GTT (topology stored locally) |
| You don't call friends daily to confirm they exist | Periodic HELLO at long intervals (150s default) |
| When you see someone, you update your mental model | Passive learning from every packet |
| "Haven't heard from X in a while" -- casual check-in | Soft expiry -- opportunistic piggybacked query |
| "X hasn't responded in months" -- assume departed | Hard expiry -- bounded verification probes |
| New person introduced by mutual friend | Mentorship bootstrap with offer collection |
| "Did you hear? Y moved away" | TC-UPDATE gossip with subject-based dedup |
| Orca pods: vocalize when hunting, silent when resting | Active network: free maintenance; idle: minimal overhead |
| Someone asks about you and you're right there | GTT-Assisted Unicast Intercept |

---

## Message Types

| Message | Type ID | Category | Purpose |
|---------|---------|----------|---------|
| E_RREQ | 1 | Routing + Topology | Route request with piggybacked topology metadata (23B, AODV wire-compatible) |
| E_RREP | 2 | Routing + Topology | Route reply with piggybacked topology metadata (19B, AODV wire-compatible) |
| E_RERR | 3 | Routing + Topology | Route error with piggybacked topology metadata |
| HELLO | 4 | Topology | Node join announcement (9B: 4B addr + 4B seqNo + 1B flags); optionally periodic |
| SYNC_OFFER | 5 | Topology | Mentor offers topology sync to new node (12B: 4B mentor + 4B gttSize + 4B mentee); **broadcast** for dampening |
| SYNC_PULL | 6 | Topology | Mentee requests topology page from mentor (8B: 4B index + 4B count) |
| SYNC_DATA | 7 | Topology | Mentor sends topology page (9B header + N * 15B per entry) |
| TC-UPDATE | 8 | Topology | Broadcast on node join/leave events (17B: 8B UUID + 4B subject + 1B event + 4B timestamp) |
| E_RREP_ACK | 9 | Routing | RREP acknowledgment for unidirectional link detection (1B reserved, mirrors AODV) |

### Topology Metadata Extension

Piggybacked on E_RREQ, E_RREP, and E_RERR messages. Also piggybacked on data packets when conditional piggybacking is active.

Wire format: `[1B count][N * (4B addr + 2B ttl + 1B flags)]` = 1 + 7N bytes.

Flags: bit 0 = `freshness_request` (soft-expiry query).

---

## Routing

AODV-based reactive routing with the following TAVRN extensions:

### Standard AODV Behavior
- Next-hop route table with sequence numbers for loop freedom
- Expanding Ring Search (TTL start=1, increment=2, threshold=7)
- RREQ/RERR rate limiting (10/s each)
- Precursor lists for RERR propagation
- Unidirectional link detection via E_RREP_ACK + blacklisting

### TAVRN Extensions

**GTT-Assisted Smart TTL**: When initiating route discovery for a destination present in the GTT with a non-expired, non-zero hop count, TAVRN skips the Expanding Ring Search and sets the initial RREQ TTL to `gttHopCount + 2`. This eliminates wasted broadcast waves (TTL=1,3,5,7) when the destination is known to be, e.g., 8 hops away. If the Smart TTL fails (no RREP received), the next retry falls back to full flood (`netDiameter`). Smart TTL is only trusted for GTT entries with positive TTL remaining (not hard-expired/stale).

**GTT-Assisted Unicast Intercept**: When forwarding an RREQ and the destination is a confirmed fresh 1-hop neighbor (in the neighbor table with positive expire time), the RREQ is unicast directly to the destination instead of being broadcast. This eliminates an entire broadcast wave for the last hop.

**Passive Topology Learning**: Every received packet -- data, control, forwarded -- updates the GTT. The previous hop (from shim tag or reverse route), the IP source, RREQ originators, RREP destinations, RERR senders, and TC-UPDATE forwarders all trigger GTT refresh. This provides continuous topology maintenance as a side effect of normal operation.

**Route-Check Bypass**: When a GTT entry reaches hard expiry but a valid route to that node still exists (ActiveRouteTimeout > GttTtl), the route itself proves reachability. The GTT entry is silently refreshed without sending a verification E_RREQ, avoiding unnecessary broadcast floods.

---

## Core Mechanisms

### 1. Initialization (HELLO / Mentorship)

A new node (N) joins the network with an empty GTT.

*Social analog: A newcomer arrives in town and is introduced to everyone by an established resident.*

**Discovery:**
- N broadcasts HELLO with `isNew=true` flag
- N starts a bootstrap timer (3s). If no SYNC_OFFER arrives, N self-bootstraps with an empty GTT and announces itself via TC-UPDATE(JOIN).

**Offer Phase:**
- Neighbors (A, B, C) receive HELLO
- Each calculates RSSI-proportional backoff: -90dBm maps to 500ms, -30dBm maps to 10ms, with linear interpolation and 0-50ms random jitter
- Neighbor A has the strongest signal, wins the race
- A **broadcasts** SYNC_OFFER (containing A's address, GTT size, and N's address as the target mentee)
- Neighbors B and C overhear A's broadcast offer and cancel their pending offers for N (broadcast dampening)
- Duplicate offer suppression: mentors track recently-offered mentees for 10s to avoid re-offering

**Offer Collection:**
- N collects SYNC_OFFERs during a 2-second window
- After the window closes, N picks the mentor with the largest GTT size
- This ensures N gets the most complete topology view, not just the fastest responder

**Pagination (Pull-based sync):**
```
N -> A:  SYNC_PULL (index: 0, count: 15)
A -> N:  SYNC_DATA (nodes 0-14, each with addr + lastSeen + ttl + seqNo + hopCount)
N -> A:  SYNC_PULL (index: 15, count: 15)
A -> N:  SYNC_DATA (nodes 15-29)
...repeat until complete...
```

Each SYNC_DATA entry is 15 bytes: 4B addr + 4B lastSeen + 2B ttl + 4B seqNo + 1B hopCount. The mentee hop count is set to mentor's hopCount + 1.

**Mentor Death Recovery:**
- Each SYNC_PULL starts a timeout timer (2x net traversal time)
- If no SYNC_DATA response arrives, retry up to 3 times
- After 3 failed retries, clear the mentor, re-broadcast HELLO, and restart bootstrap

N now has full topology awareness and can participate in GTT maintenance.

### 2. Steady State (Dual-TTL Refresh)

Each GTT entry has a Time-To-Live with two expiry classes:

*Social analog: You don't call friends daily, but if you haven't heard from someone in a while, you might ask around. If they've been silent for months, you assume they've moved away.*

**Soft Expiry (TTL reaches soft threshold, default 1/2):**
- On next **control message** (E_RREQ/E_RREP/E_RERR), always append metadata including freshness requests for soft-expired entries
- On **data packet forwarding**, only piggyback metadata when soft-expired entries exist (conditional piggybacking). When topology is stable, data packets are forwarded clean (AODV-sized)
- Entries are selected via round-robin cursor to prevent address-ordered starvation
- A per-entry cooldown (default 5s) prevents redundant re-propagation of the same entry

**Soft Expiry Response Prioritization (Tiered):**
- **Tier 1**: Target node itself responds immediately (it IS the proof of existence)
- **Tier 2**: Nodes with TTL > 2x requester's TTL respond after short backoff (10-100ms)
- **Tier 3**: Nodes with marginally higher TTL suppress response (let fresher sources answer)

**Hard Expiry (TTL reaches 0):**
- First check: if a valid route exists to the node (ActiveRouteTimeout > GttTtl), silently refresh the GTT entry (route proves reachability)
- If no route: begin bounded probe-based verification
  - Send targeted E_RREQ to verify node existence
  - Wait 2x net traversal time between probes
  - Maximum 2 retry probes (3 total attempts including initial)
  - Response received at any point: refresh GTT entry, cancel verification
  - No response after all retries: mark node as departed, broadcast TC-UPDATE(NODE_LEAVE)

**Key property**: In active networks, soft expiry resolves via piggybacked metadata. Standalone control packets only occur at hard expiry, and even then are suppressed when valid routes exist.

### 3. Passive Topology Learning

Every received packet provides liveness evidence, even without piggybacked metadata:

- **Data packets with shim tag**: Strip TopologyMetadataHeader, process entries, refresh previous hop in GTT
- **Data packets without shim tag** (clean, v2.1): Refresh previous hop and source IP in GTT
- **E_RREQ**: Refresh originator (with seqNo and hopCount from header) and sender (1-hop)
- **E_RREP**: Refresh destination (with seqNo and hopCount) and sender (1-hop)
- **E_RERR**: Refresh sender (1-hop)
- **TC-UPDATE**: Refresh sender (1-hop)
- **Forwarding path**: Refresh both source and destination via routing table hop counts

This provides continuous, free GTT maintenance as a side effect of normal network operation.

### 4. Topology Change Broadcast (TC-UPDATE)

On confirmed node join or leave:

*Social analog: "Did you hear? The Smiths moved away last week."*

- Originator broadcasts TC-UPDATE with unique UUID and timestamp
- UUID = originator address (32-bit) + local sequence number (32-bit) = 64-bit
- Recipients:
  1. Check UUID against dedup cache (30s lifetime). If seen, drop
  2. Check subject-based dedup cache (1s window). Squashes redundant floods when multiple neighbors detect the same node death simultaneously
  3. Update local GTT (add on JOIN, mark departed on LEAVE)
  4. Refresh sender in GTT (passive learning)
  5. Fire convergence trace
  6. Forward with decremented TTL (not reset to netDiameter)
- Forwarding serves as implicit acknowledgment per gossip protocol convention (no explicit ACK)

**Subject-Based Dedup**: When multiple neighbors detect the same node failure (e.g., via MAC layer retry limit), they all generate TC-UPDATE(NODE_LEAVE) for the same subject. The subject-based dedup cache (keyed on {subject, event_type} with 1s window) squashes these redundant floods, preventing O(k) amplification where k is the number of detecting neighbors.

---

## Conditional Piggybacking (v2.1 Improvement)

TAVRN v2.1 introduces conditional piggybacking on data packets:

**Mode A (Stable topology):** When no GTT entries are soft-expired, data packets are forwarded clean -- no TopologyMetadataHeader, no TavrnShimTag. Packets are AODV-sized. This is the common case in stable networks.

**Mode B (Topology refresh needed):** When soft-expired entries exist, a TopologyMetadataHeader is prepended to the data packet and a TavrnShimTag is attached. The next hop strips the metadata, processes it for GTT updates, and passes the clean packet onward.

**Shim mechanism:** The TavrnShimTag carries the previous hop's IP address (4 bytes). This is necessary because on multi-hop paths, the IP header source is the flow origin, not the node that piggybacked the metadata. The shim tag ensures correct attribution.

Control messages (E_RREQ/E_RREP/E_RERR) always carry topology metadata regardless of topology stability, since they are already TAVRN-specific overhead.

---

## Tunability

TAVRN can be configured across the 0.2--0.5 reactivity spectrum via ns-3 Attributes:

| Parameter | Attribute Name | Effect | Toward Reactive (0.2) | Toward Proactive (0.5) |
|-----------|---------------|--------|----------------------|------------------------|
| GTT-TTL | `GttTtl` | Freshness tolerance | Long (300s) | Short (30s) |
| Soft-expiry threshold | `SoftExpiryThreshold` | When opportunistic refresh triggers | Late (0.25) | Early (0.75) |
| Periodic HELLO | `EnablePeriodicHello` | Keepalive frequency | Disabled | Enabled |
| HELLO interval | `HelloInterval` | Time between HELLOs | Long (5min) | Short (10s) |
| Max metadata entries | `MaxMetadataEntries` | Entries piggybacked per message | Low (3) | High (8) |
| Piggybacking cooldown | `PiggybackCooldown` | Re-propagation suppression | Long (10s) | Short (1s) |
| TC-UPDATE expiry | `TcUpdateExpiryWindow` | UUID dedup cache lifetime | Short (15s) | Long (60s) |

### Implementation Default Profile

The ns-3 implementation uses these defaults, tuned for IoT/smart-home networks:

```
# Implementation defaults (balanced for smart home)
GttTtl              = 300s
SoftExpiryThreshold = 0.5
EnablePeriodicHello = true
HelloInterval       = 150s    # 0.5 * GttTtl
MaxMetadataEntries  = 5
PiggybackCooldown   = 5s
TcUpdateExpiryWindow = 30s
ActiveRouteTimeout  = 600s    # 2 * GttTtl (routes outlive GTT entries)
NetDiameter         = 35
NodeTraversalTime   = 40ms
RreqRetries         = 2
AllowedHelloLoss    = 2
```

### Adaptive GTT TTL (Optional)

When enabled via `EnableAdaptiveGttTtl = true`, the GTT TTL becomes dynamic rather than static. This mode is designed for Layer 2 environments that cannot report link failures in a timely manner (e.g., no MAC-layer TX failure callbacks). When L2 provides reliable failure reporting, the static TTL (default) is preferred for its lower overhead.

**Tier 1 — Global TTL:**

The global GTT TTL starts at `GttTtlMin` (default 60s) and grows toward `GttTtlMax` (default 300s) via exponential moving average (EMA) each maintenance cycle, provided the neighbor count has not changed:

```
TTL_new = alpha * TTL_old + (1 - alpha) * TTL_max
```

On direct neighbor gain or loss (detected by comparing neighbor count between maintenance ticks), the global TTL resets to `GttTtlMin` for fast re-convergence.

All derived timers scale dynamically: `HelloInterval = 0.5 * TTL`, `ActiveRouteTimeout = 2 * TTL`.

**Tier 2 — Per-Node TTL:**

Each GTT entry has an optional per-node TTL override. When a remote TC-UPDATE NODE_JOIN is received (new or resurrected node), the per-node TTL for that entry snaps to `GttTtlMin` for fast tracking of the new node. Per-node TTLs grow toward the current global TTL via the same EMA each maintenance tick, and are always clamped to be <= the global TTL.

**Adaptive TTL parameters:**

| Parameter | Attribute | Default | Description |
|-----------|-----------|---------|-------------|
| Enable | `EnableAdaptiveGttTtl` | `false` | Whether adaptive TTL is active |
| TTL floor | `GttTtlMin` | 60s | Fast-mode TTL during bootstrap/churn |
| TTL ceiling | `GttTtlMax` | 300s | Steady-state TTL ceiling |
| EMA alpha | `GttAlpha` | 0.7 | Smoothing factor (higher = slower growth) |

**When to use adaptive TTL:**

- L2 has no MAC TX failure callback (no `NotifyTxError` equivalent)
- Network experiences frequent topology changes without L2 notification
- Faster GTT convergence after topology changes is worth the overhead increase (~15% more control traffic)

### Configuration Profiles

```
# Near-AODV (0.2): Maximum silence, eventual consistency
GttTtl              = 300s
SoftExpiryThreshold = 0.25
EnablePeriodicHello = false

# Balanced (0.35): Good trade-off
GttTtl              = 120s
SoftExpiryThreshold = 0.5
EnablePeriodicHello = true
HelloInterval       = 60s

# Near-OLSR (0.5): Faster convergence, more overhead
GttTtl              = 30s
SoftExpiryThreshold = 0.75
EnablePeriodicHello = true
HelloInterval       = 10s
```

### Derived Timer Relationships

Several timers are computed from base parameters:

| Derived Timer | Formula | Purpose |
|---------------|---------|---------|
| ActiveRouteTimeout | 2 * GttTtl | Routes outlive GTT entries for route-check bypass |
| HelloInterval | 0.5 * GttTtl (default) | Soft-expiry duty cycle |
| NetTraversalTime | 2 * NetDiameter * NodeTraversalTime | RREQ round-trip bound |
| PathDiscoveryTime | 2 * NetTraversalTime | RREQ ID cache lifetime |
| BlackListTimeout | RreqRetries * NetTraversalTime | Unidirectional link blacklist |
| GttMaintenanceInterval | max(1s, GttTtl / 12) | Proportional to TTL for radio silence |

---

## Steady-State Overhead

| Condition | Standalone Control Packets |
|-----------|---------------------------|
| Network active, topology stable | **Zero** (conditional piggybacking is dormant; data packets are clean) |
| Network active, topology refreshing | **Zero** (metadata piggybacked on existing data/control traffic) |
| Network idle, topology stable | Periodic HELLO at configured interval (if enabled) |
| Network idle, topology decaying | Soft-expiry requests piggybacked on next outgoing traffic; HELLOs if no traffic |
| Topology change (node join/leave) | Single TC-UPDATE flood (TTL-bounded, subject-deduplicated) |
| Hard expiry, valid route exists | **Zero** (route-check bypass silently refreshes GTT) |
| Hard expiry, no route | 1-3 targeted E_RREQ verification probes per expiring entry |

---

## GTT Interface (For Applications)

The GTT exposes a query API for application layers (service discovery, Matter, CoAP):

```cpp
// Query topology
bool                    NodeExists(Ipv4Address addr);
Time                    LastSeen(Ipv4Address addr);
Time                    TtlRemaining(Ipv4Address addr);
std::vector<Ipv4Address> EnumerateNodes();
uint32_t                NodeCount();

// Event subscriptions (TracedCallbacks)
TracedCallback<Ipv4Address>  m_nodeJoinTrace;   // New node discovered
TracedCallback<Ipv4Address>  m_nodeLeaveTrace;  // Node departed
TracedCallback<uint32_t>     m_sizeChangeTrace; // Active count changed

// Configuration
void    SetDefaultTtl(Time ttl);
void    SetSoftExpiryThreshold(double ratio);  // [0.0, 1.0]

// Bulk operations (used during mentorship)
std::vector<GttEntry>   GetEntriesPage(uint32_t startIndex, uint32_t count);
void                    MergeEntry(const GttEntry& entry);
```

The GTT is passive -- it does not schedule timers or send packets. All actions are driven by the owning RoutingProtocol.

---

## GTT Entry Structure

Each entry in the Global Topology Table:

| Field | Type | Description |
|-------|------|-------------|
| nodeAddr | IPv4 | Address of the known node |
| lastSeen | Time | Absolute simulation time of last evidence |
| ttlExpiry | Time | Absolute time of hard expiry |
| softExpiry | Time | Absolute time of soft expiry (triggers freshness request) |
| seqNo | uint32 | Latest known sequence number |
| hopCount | uint16 | Estimated hop distance |
| departed | bool | True if confirmed departed (awaiting purge) |

Departed entries are retained for `2 * GttTtl` after departure to give TC-UPDATE time to propagate, then garbage collected by `Purge()`.

---

## Failure Modes and Mitigations

| Failure | Behavior | Mitigation |
|---------|----------|------------|
| Network goes quiet | Soft expiries accumulate, triggering verification traffic | Acceptable: verification is distributed over time; route-check bypass absorbs many |
| Traffic is localized (A<->B active, C silent) | C's view of A and B stays fresh via passive learning; A and B's view of C decays | C will be verified at hard expiry; periodic HELLO prevents this in default config |
| Partition heals | Nodes have divergent GTTs | TC-UPDATEs propagate; full resync via mentorship if drift is severe |
| High churn | Frequent TC-UPDATEs | TAVRN is not designed for high churn; subject-based dedup reduces storm amplification |
| Mentor death during bootstrap | SYNC_PULL times out | 3 retries, then clear mentor, re-broadcast HELLO, restart bootstrap |
| Multiple mentors race | Redundant SYNC_OFFERs | Broadcast dampening: overheard offers cancel pending ones |
| MAC retry limit reached | Link declared broken | Immediate TC-UPDATE(NODE_LEAVE) + RERR, not waiting for GTT expiry |
| False departure (queue drop, not link failure) | Node incorrectly marked departed | Only REACHED_RETRY_LIMIT triggers departure; queue management drops (FAILED_ENQUEUE, EXPIRED_LIFETIME) are ignored |

---

## Complexity Analysis

| Metric | Complexity | Notes |
|--------|------------|-------|
| GTT memory | O(N) per node | N = network size; unavoidable for topology awareness |
| Routing table | O(active destinations) | Standard AODV behavior |
| Message overhead (active, stable) | O(0) per data message | Conditional piggybacking: clean when stable |
| Message overhead (active, refreshing) | O(1) per data message | Fixed metadata size (1 + 7*MaxEntries bytes) |
| Message overhead (idle) | O(N / GttTtl) | Amortized verification, reduced by route-check bypass |
| Convergence (topology change) | O(diameter) | TC-UPDATE flood with TTL decrement |
| Bootstrap | O(N / SyncPageSize) | Paginated pull; 15 entries/page default |

---

## Optional Extension: Protocol Duality (Multi-Radio)

For devices with heterogeneous radios (BLE + WiFi, BLE + ESP-NOW + WiFi), TAVRN's message diversity maps naturally to radio selection:

| Message | Characteristics | Suggested Radio |
|---------|-----------------|-----------------|
| HELLO | Small (10B), broadcast, announce | BLE (low power, always-on) |
| TC-UPDATE | Small (18B), broadcast, urgent | BLE |
| E_RREQ | Small (24B+), broadcast, discovery | BLE / ESP-NOW |
| E_RREP, E_RERR | Small, unicast, latency-sensitive | ESP-NOW |
| SYNC_PULL/DATA | Large, unicast, bulk | WiFi |
| Application data | Variable, throughput | WiFi |

**Note on BLE Mesh**: BLE Mesh advertising bearers have only 11 bytes of application payload -- TAVRN piggybacking is physically impossible on this transport. GATT bearer connections would work but add latency. 802.15.4 (Zigbee/Thread) has 127-byte frames (~80B usable payload), which is tight but feasible for TAVRN metadata (7-35B per piggybacked extension).

This extension is separable and can be applied to other protocols independently.

---

## Comparison with Existing Protocols

| Dimension | AODV | TAVRN | OLSR | DSDV |
|-----------|------|-------|------|------|
| Routing | Reactive | Reactive + GTT-assisted | Proactive | Proactive |
| Topology awareness | None | Full (GTT) | Full (link-state) | Full (distance-vector) |
| Idle overhead | Zero | Periodic HELLO (configurable) | Continuous TC/HELLO | Continuous updates |
| Active overhead | Route discovery | Discovery + conditional metadata | Full link-state | Incremental updates |
| Freshness guarantee | N/A | Probabilistic (tunable) | Bounded by TC interval | Bounded by update interval |
| Memory per node | O(routes) | O(N) | O(N^2) links | O(N) |
| Reactivity range | 0.0 | 0.2 -- 0.5 | 0.5 -- 1.0 | 0.8 -- 1.0 |
| Smart route discovery | No (ERS only) | Yes (GTT hop count) | N/A (proactive) | N/A (proactive) |
| Unidirectional link detection | Yes (RREP_ACK) | Yes (E_RREP_ACK) | Yes (2-hop) | No |
| Node enumeration | No | Yes (`EnumerateNodes()`) | Yes (topology set) | Yes (routing table) |

---

## ns-3 Implementation Notes

The reference implementation targets ns-3.45 using WiFi (802.11) + IPv4 for simulation. This provides fair comparison against ns-3's built-in AODV, OLSR, and DSDV implementations.

**Why WiFi, not BLE/802.15.4**: ns-3's BLE and 802.15.4 modules lack the PHY-level tracing and mature MANET support needed for rigorous comparison. WiFi results proxy well for constrained networks: AODV results proxy for Zigbee's AODV-derived mesh routing; OLSR results proxy for Thread/RPL's proactive approach.

**Key implementation files:**

| File | Purpose | Lines |
|------|---------|-------|
| `tavrn-routing-protocol.cc/h` | Core protocol: routing, GTT maintenance, mentorship, piggybacking | ~5200 |
| `tavrn-gtt.cc/h` | Global Topology Table: passive data structure with dual-TTL | ~900 |
| `tavrn-packet.cc/h` | All 9 message headers with wire format serialization | ~1700 |
| `tavrn-neighbor.cc/h` | Neighbor management with per-neighbor RSSI caching | ~400 |
| `tavrn-rtable.cc/h` | AODV-style routing table | ~400 |
| `tavrn-test-suite.cc` | 21 unit tests | ~1650 |
| `tavrn-comparison.cc` | Benchmark harness: 17 scenarios, multi-seed sweep, 95% CI | ~1400 |

---

## Summary

TAVRN v2.1 is a mesh network protocol occupying the underserved space between pure reactive routing (AODV) and full proactive link-state (OLSR). By treating topology as local knowledge maintained through social-style relationship tracking, it achieves:

- **Full topology visibility** for service discovery layers
- **Near-zero steady-state overhead** in active, stable networks (conditional piggybacking)
- **Tunable reactivity** from near-AODV silence to near-OLSR freshness
- **Explicit resource trade-off**: memory and CPU for radio silence
- **Smart route discovery**: GTT-assisted TTL skips expanding ring search
- **Graceful degradation**: route-check bypass, bounded verification, subject-based dedup

TAVRN is designed for medium-sized, relatively stable MANETs where topology awareness is required but continuous control traffic is unacceptable.

> *The network that gossips when gathering, and rests in comfortable silence.*

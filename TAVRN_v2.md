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

## Addressing: Entropy-Based Suffix Compression (ESC)

TAVRN compresses L3 addresses on the wire using **Entropy-Based Suffix Compression (ESC)**, inspired by IPHC (RFC 6282) but adapted for TAVRN's unique architecture. ESC is PHY-agnostic: it works identically over IPv4, IPv6, and 802.15.4 without protocol changes.

### Motivation

Constrained mesh protocols universally compress addresses on the wire:

| Protocol | Wire Address | Size | Context Source |
|----------|-------------|------|----------------|
| 6LoWPAN (RFC 6282) | IPHC-compressed IPv6 | 0/16/64-bit | Shared context table (management out of scope) |
| 802.15.4 / Zigbee | PAN-local short address | 16-bit | Coordinator assigns during join |
| Thread | RLOC16 (Router ID + Child ID) | 16-bit | Leader assigns Router ID |
| Bluetooth Mesh | Unicast address | 16-bit | Provisioner assigns during provisioning |
| Z-Wave | Node ID | 8-bit | Controller assigns during inclusion |
| **TAVRN** | **Entropy-compressed suffix** | **1/2/8/16 B** | **GTT (already exists)** |

Every protocol above requires an external mechanism to establish shared addressing context: a PAN coordinator, a leader, a provisioner, or an unspecified "context management" layer. **TAVRN already has this mechanism** -- the GTT. Every node maintains a synchronized topology view through mentorship and piggybacking. ESC leverages the GTT as the shared context table, requiring zero additional infrastructure.

### Core Concept: Entropy from GTT

**Entropy** is the minimum number of trailing address bytes required to uniquely identify every node in the GTT.

Given a set of L3 addresses `{a1, a2, ..., aN}` in a node's GTT, the entropy `k` is:

```
k = min { j in [1, addrLen] : suffix(ai, j) != suffix(ak, j) for all i != k }
```

Where `suffix(addr, j)` returns the last `j` bytes of the address.

**Examples**:
- `/24` subnet (10.1.1.0/24): all addresses differ in the last byte. `k = 1`.
- `/16` multi-subnet (10.1.1.5, 10.1.2.5): last byte collides. `k = 2`.
- IPv6 with shared prefix: `k` depends on the interface ID diversity.

The entropy computation is **deterministic**: given the same GTT contents, every node computes the same `k`. Since TAVRN's mentorship and piggybacking mechanisms converge the GTT across all nodes, entropy agreement follows naturally from GTT convergence.

### Address Mode (AM) Field

Inspired by IPHC's SAM/DAM encoding, each address field in a TAVRN header carries a **2-bit Address Mode (AM)** that tells the receiver how many bytes follow:

```
AM  Inline Bytes  Usage
--  ------------  -----
11  1 byte         Suffix-1: covers /24 networks, most IoT deployments
10  2 bytes        Suffix-2: covers /16 networks, multi-subnet meshes
01  8 bytes        Suffix-8: IPv6 interface ID (prefix elided)
00  16 bytes       Full address: IPv6 (128-bit) or IPv4 (4 bytes, zero-padded)
```

The AM bits are packed into existing flag/control bytes in each header (see Message Types). For headers with two address fields (e.g., E_RREQ: origin + destination), both AM values are packed into a single byte alongside other flags.

**Key difference from IPHC**: RFC 6282 states *"How the information is maintained in that shared context is out of scope."* In TAVRN, the context source is explicitly defined: it is the GTT, synchronized via mentorship (SYNC_OFFER/SYNC_PULL/SYNC_DATA) and maintained via piggybacking.

### Sender Behavior

1. **Compute entropy** `k` from own GTT: find the minimum suffix bytes for all known addresses to be unique.
2. **Select AM** for each address field based on `k`:
   - `k = 1` -> `AM = 11` (1 byte)
   - `k = 2` -> `AM = 10` (2 bytes)
   - `k <= 8` -> `AM = 01` (8 bytes)
   - Otherwise -> `AM = 00` (full address)
3. **Exception**: if the address is **not in the sender's GTT** (unknown node, e.g., RREQ for a node discovered via application hint), use `AM = 00` (full address). You cannot suffix-compress what you have not verified as unique.

### Receiver Behavior

1. **Read AM bits** from the header, read the corresponding number of inline bytes.
2. **Look up the suffix** in the local GTT:
   - **Unique match**: resolved. Proceed normally.
   - **No match**: unknown node. Send E_RERR (standard "destination unreachable" behavior).
   - **Multiple matches (ambiguous)**: the sender's entropy is stale. See Collision Handling below.

### Collision Handling

A suffix collision occurs when a receiver's GTT contains multiple nodes whose addresses share the same trailing `k` bytes. This can only happen when the sender's GTT is missing a node that the receiver knows about (i.e., the sender's entropy is stale).

**Detection**: receiver decodes a suffix and finds >1 GTT match.

**Response**: E_RERR with the **Ambiguity flag (A=1)** set (see E_RERR flags). The RERR carries the conflicting full addresses as topology metadata via normal piggybacking.

**Recovery**: the sender receives the RERR, updates its GTT with the new topology information (piggybacked metadata), recomputes entropy (which increases), and retries the route discovery with a higher AM.

```
Example collision flow:

Network: 10.1.1.0/24 + 10.1.2.0/24 (two subnets)
Node B (stale GTT, k=1) -> E_RREQ [dest=0x05, AM=11]
Node A (full GTT, k=2)  -> suffix 0x05 matches 10.1.1.5 AND 10.1.2.5
Node A -> E_RERR [A=1, topology metadata includes both full addresses]
Node B -> updates GTT, k bumps to 2
Node B -> E_RREQ [dest=0x0105, AM=10] -> resolves unambiguously
```

This mirrors 6LoWPAN's fallback to full EUI-64 addresses on conflict, but TAVRN detects and resolves collisions automatically through existing error recovery mechanisms rather than requiring coordinator intervention.

### Entropy Lifecycle

- **Initialization**: `k` starts at 1 (minimum) when the GTT is first populated via mentorship.
- **Increase**: immediate when a new node joins whose address collides at the current suffix length. The GTT update (via TC_UPDATE or mentorship) triggers recomputation.
- **Decrease**: uses a **sticky high-water mark with slow decay**. Once `k` increases, it remains at the higher value for `10 * GTT_TTL` before decaying. This prevents oscillation when nodes join/leave rapidly. The rationale: increasing entropy is cheap (just send more bytes), but decreasing it risks ambiguity if the GTT hasn't fully converged after a departure.
- **Reset**: on full node restart, `k` recomputes from the fresh GTT received during mentorship.

### Bootstrap Addressing

During bootstrap, a new node has no GTT and cannot compute entropy:

- **HELLO**: always uses `AM = 00` (full L3 address). The new node announces itself with its complete address so existing nodes can add it to their GTTs.
- **SYNC_OFFER**: mentor uses `AM = 00` for the mentee's address (the mentee may not know the network's entropy yet).
- **SYNC_DATA**: always uses `AM = 00` (full L3 addresses) for all GTT entries. SYNC_DATA builds the mentee's GTT from scratch; the mentee needs full addresses to populate its context table.
- **Post-mentorship**: once the mentee has a populated GTT, it computes `k` and switches to suffix compression for all subsequent messages.

### Wire Format Impact

| Field | Uncompressed (IPv4) | ESC (k=1) | ESC (k=2) |
|-------|---------------------|-----------|-----------|
| Node address | 4 bytes | 1 byte | 2 bytes |
| Sequence number | 4 bytes | 2 bytes | 2 bytes |
| Request ID | 4 bytes | 2 bytes | 2 bytes |
| Lifetime | 4 bytes | 2 bytes (non-linear) | 2 bytes (non-linear) |
| TTL (metadata) | 2 bytes | 4-bit bucket | 4-bit bucket |
| Timestamp | 4 bytes | 2 bytes | 2 bytes |

Address compression is the primary saving. Other field compressions (sequence numbers, lifetimes, TTL buckets) are applied uniformly regardless of entropy level.

### ns-3 Simulation Mapping

In the ns-3 simulation environment (WiFi + IPv4 on a 10.1.1.0/24 subnet):
- Entropy is always `k = 1` (last octet uniquely identifies each node)
- All address fields use `AM = 11` (1-byte suffix)
- `suffix = ipv4_addr.Get() & 0xFF` (last octet)
- `full_addr = network_prefix | suffix` (reverse mapping)
- The network prefix is set once during `RoutingProtocol::Start()`

This is not a simplification -- it is the correct ESC behavior for a /24 network. A multi-subnet ns-3 scenario would naturally produce `k = 2` with zero code changes.

---

## Architecture

```
+-----------------------------------------------------------+
|  Application / Service Discovery (Matter, CoAP, etc.)     |
+-----------------------------------------------------------+
|  TAVRN v2.2                                               |
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

All headers use ESC (Entropy-Based Suffix Compression) for address fields on the wire. Each address field is preceded by a 2-bit AM (Address Mode) packed into the header's flags byte. Sequence numbers are 16-bit. See "Addressing: Entropy-Based Suffix Compression (ESC)" for details.

Wire sizes below are shown for `k=1` (1-byte suffixes, typical /24 IoT network). Sizes scale with entropy: at `k=2`, each address field adds 1 byte.

| Message | Type ID | Category | Wire Size (k=1) | Purpose |
|---------|---------|----------|------------------|---------|
| E_RREQ | 1 | Routing + Topology | **10B** | Route request: `[1B flags+AM][1B hop][2B reqId][kB dst][2B dstSeq][kB origin][2B originSeq]` |
| E_RREP | 2 | Routing + Topology | **8B** | Route reply: `[1B flags+AM][1B hop][kB dst][2B dstSeq][kB origin][2B lifetime]` |
| E_RERR | 3 | Routing + Topology | **1 + 3N** | Route error: `[1B flags+A+count][N * (kB dst + 2B seqNo)]`. **A flag**: address ambiguity (see ESC Collision Handling) |
| HELLO | 4 | Topology | **4B** | Node announcement: `[kB node][2B seqNo][1B flags]` (bootstrap: AM=00, full address) |
| SYNC_OFFER | 5 | Topology | **3B** | Mentor offers sync: `[kB mentor][1B gttSize][kB mentee]` |
| SYNC_PULL | 6 | Topology | **2B** | Request page: `[1B index][1B count]` |
| SYNC_DATA | 7 | Topology | **3 + 7N** | Topology page: `[1B start][1B total][1B count][N * (L3B fullAddr + 2B lastSeen + 1B ttlBucket + 2B seqNo + 1B hop)]`. Always AM=00 (full addresses) |
| TC-UPDATE | 8 | Topology | **7B** | Join/leave: `[kB origin][2B seqNo][kB subject][1B event][2B timestamp]` |
| E_RREP_ACK | 9 | Routing | **1B** | RREP acknowledgment (1B reserved, mirrors AODV) |

### E_RERR Flags Byte

```
 0   1   2   3   4   5   6   7
+---+---+---+---+---+---+---+---+
| N |  AM_d | A |   destCount   |
+---+---+---+---+---+---+---+---+

N (1 bit):    No-delete flag (standard AODV behavior)
AM_d (2 bits): Address Mode for destination entries
A (1 bit):    Ambiguity flag -- set when RERR is caused by suffix collision
              (receiver detected multiple GTT matches for a compressed address).
              Signals sender to increase entropy and retry.
destCount (4 bits): Number of unreachable destinations (0-15)
```

### Size Comparison with AODV (IPv4)

| Packet | AODV (IPv4) | TAVRN (k=1) | TAVRN (k=2) | Ratio (k=1) |
|--------|-------------|-------------|-------------|-------------|
| RREQ bare | 24B | 10B | 12B | 0.42x |
| RREP | 20B | 8B | 10B | 0.40x |
| RERR (1 dest) | 12B | 4B | 5B | 0.33x |
| RREQ + 4 metadata | n/a | 19B | 23B | 0.79x vs AODV RREQ |

TAVRN's E_RREQ with full topology piggybacking (4 entries) at k=1 is **5 bytes smaller** than a bare AODV RREQ. At k=2, it is still 1 byte smaller.

### Topology Metadata Extension

Piggybacked on E_RREQ, E_RREP, and E_RERR messages. Also piggybacked on data packets when conditional piggybacking is active.

Wire format: `[1B count+AM][N * (kB suffix + 1B packed{4-bit TTL bucket | 4-bit flags})]` = **1 + (k+1)N bytes**.

The AM for metadata entries is shared (all entries in one extension use the same AM, packed into the count byte).

TTL bucket encoding: `bucket = min(15, ttl_seconds / 20)`, decode: `ttl = bucket * 20`. Max encodable: 300s.

Flags (lower 4 bits): bit 0 = `freshness_request` (soft-expiry query).

### Lifetime Encoding (E_RREP)

Route lifetimes are encoded in 2 bytes using non-linear encoding:
- Values 0--16383ms: stored directly (bit 15 = 0)
- Values > 16383ms: bit 15 = 1, bits 0-14 = ms / 100 (max ~3276.7s)

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

Each SYNC_DATA entry is 7 bytes at k=1: kB suffix + 2B lastSeen + 1B ttlBucket + 2B seqNo + 1B hopCount. SYNC_DATA always uses AM=00 (full L3 addresses) to build the mentee's GTT from scratch; at AM=00 with IPv4, each entry is 10 bytes (4B addr + 2B + 1B + 2B + 1B). The mentee hop count is set to mentor's hopCount + 1.

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

### Adaptive HELLO Interval (EMA-based)

The HELLO interval is decoupled from GttTtl and managed independently via exponential moving average (EMA). This provides fast convergence during bootstrap and topology changes while minimizing overhead in steady state.

**Mechanism:**

The HELLO interval starts at `HelloIntervalMin` (default 24s) and grows toward `HelloIntervalMax` (default 120s) via EMA each time a HELLO fires:

```
Interval_new = alpha * Interval_old + (1 - alpha) * Interval_max
```

When the interval reaches 95% of max, it snaps to the ceiling. On direct neighbor gain or loss (detected by comparing neighbor count between GTT maintenance ticks), the interval resets to `HelloIntervalMin` and the HELLO timer is immediately rescheduled for fast re-convergence.

GttTtl remains static (default 300s). ActiveRouteTimeout = 1.2 * GttTtl.

**Neighbor liveness detection:**

During each GTT maintenance cycle, `CheckNeighborLiveness` iterates all 1-hop neighbors and checks each neighbor's GTT `lastSeen` timestamp. If `now - lastSeen > livenessTimeout`, the neighbor is marked departed and a TC-UPDATE(NODE_LEAVE) is broadcast.

The liveness timeout uses a hysteresis floor to prevent false departures when the HELLO interval resets from a long cadence to a short one:

```
livenessTimeout = max(3 * currentHelloInterval, livenessFloor)
```

The floor is set to the previous `3 * interval` when a reset occurs and decays by half each maintenance cycle until it reaches the current threshold.

**Adaptive HELLO parameters:**

| Parameter | Attribute | Default | Description |
|-----------|-----------|---------|-------------|
| Min interval | `HelloIntervalMin` | 24s | Fast-mode HELLO during bootstrap/churn |
| Max interval | `HelloIntervalMax` | 120s | Steady-state HELLO ceiling |
| EMA alpha | `HelloAlpha` | 0.8 | Smoothing factor (higher = slower growth) |
| Enable | `EnablePeriodicHello` | `true` | Whether periodic HELLOs are active |

**Design rationale (replaces Adaptive GTT TTL):**

The previous adaptive GTT TTL mechanism caused a self-sabotaging feedback loop: neighbor count changes reset the GTT TTL from steady-state to minimum, which shortened all derived timers (HELLO, ActiveRouteTimeout), causing more frequent expiry, more verification, more churn, and more resets. The TTL never reached steady state. Decoupling HELLO from GttTtl eliminates this loop while preserving fast failure detection.

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

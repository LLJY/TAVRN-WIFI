# TAVRN v2.2 Implementation Guide

This document explains how TAVRN is implemented as an ns-3 module. It maps every spec concept to concrete code — classes, methods, wire formats, and data flows — so you can explain the protocol confidently at any level of detail.

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Source File Map](#2-source-file-map)
3. [Entropy-Based Suffix Compression (ESC)](#3-entropy-based-suffix-compression-esc)
4. [Wire Formats](#4-wire-formats)
5. [Global Topology Table (GTT)](#5-global-topology-table-gtt)
6. [Routing Table](#6-routing-table)
7. [Neighbor Table](#7-neighbor-table)
8. [Routing Protocol Core](#8-routing-protocol-core)
9. [Lifecycle Walkthroughs](#9-lifecycle-walkthroughs)
10. [Key Constants and Tunable Parameters](#10-key-constants-and-tunable-parameters)
11. [Design Decisions and Trade-offs](#11-design-decisions-and-trade-offs)

---

## 1. Executive Summary

TAVRN (Topology Aware Vicinity-Reactive Network) is a hybrid mesh routing protocol that sits between purely reactive protocols (AODV) and purely proactive ones (OLSR/DSDV). Its core insight is that in real IoT deployments, data traffic already flows between nodes constantly — so topology maintenance can piggyback on that traffic for free, instead of requiring dedicated control messages.

The implementation comprises five key components:

| Component | File(s) | Responsibility |
|---|---|---|
| **ESC + Packet Headers** | `tavrn-packet.{h,cc}` | Wire encoding: every address is compressed to 1-byte suffix using GTT as shared context |
| **GTT** | `tavrn-gtt.{h,cc}` | Passive topology store: who exists, when last seen, how far away |
| **Routing Table** | `tavrn-rtable.{h,cc}` | AODV-style next-hop forwarding table with precursors and blacklists |
| **Neighbor Table** | `tavrn-neighbor.{h,cc}` | 1-hop neighbor liveness, MAC resolution, RSSI tracking |
| **Routing Protocol** | `tavrn-routing-protocol.{h,cc}` | Orchestrator: owns all timers, decisions, and transmissions (~4500 lines) |

**What makes TAVRN different from AODV:**
- **GTT**: every node knows the full network membership (who exists, not just how to reach them)
- **ESC**: all control packets use 1-byte addresses instead of 4-byte IPv4, cutting wire overhead by ~60%
- **Piggybacking**: topology metadata rides on existing data/control traffic — no dedicated flooding
- **Mentorship**: new nodes get the full topology table from a neighbor in seconds, not through network-wide flooding
- **Smart TTL**: route discovery uses GTT hop estimates to set initial RREQ TTL, avoiding unnecessary broadcast waves

---

## 2. Source File Map

```
src/tavrn/
├── model/
│   ├── tavrn-packet.h          # ESC encoding + all 9 header types
│   ├── tavrn-packet.cc         # Serialization/deserialization
│   ├── tavrn-gtt.h             # GttEntry struct + GlobalTopologyTable class
│   ├── tavrn-gtt.cc            # GTT operations, jitter, dual-expiry
│   ├── tavrn-rtable.h          # RoutingTableEntry + RoutingTable
│   ├── tavrn-rtable.cc         # Route CRUD, precursors, purge
│   ├── tavrn-neighbor.h        # Neighbor struct + Neighbors class
│   ├── tavrn-neighbor.cc       # Neighbor liveness, ARP, RSSI cache
│   ├── tavrn-routing-protocol.h # RoutingProtocol class declaration
│   └── tavrn-routing-protocol.cc # The orchestrator (~4500 lines)
├── helper/
│   └── tavrn-helper.{h,cc}     # ns-3 helper for installing TAVRN on nodes
├── test/
│   └── tavrn-test-suite.cc     # 21 unit tests for all header types + GTT
└── CMakeLists.txt
```

---

## 3. Entropy-Based Suffix Compression (ESC)

### 3.1 The Problem ESC Solves

Every constrained mesh protocol compresses addresses on the wire:

| Protocol | Mechanism | Context Source |
|---|---|---|
| 6LoWPAN (RFC 6282) | IPHC | Shared context table (management unspecified) |
| Zigbee | PAN-local short address | Coordinator assigns during join |
| Thread | RLOC16 | Leader assigns Router ID |
| Bluetooth Mesh | Unicast address | Provisioner assigns |
| Z-Wave | Node ID | Controller assigns |
| **TAVRN** | **Entropy-compressed suffix** | **GTT (already exists)** |

Every protocol above needs an external mechanism (coordinator, leader, provisioner) to establish shared addressing context. **TAVRN already has this** — the GTT. Every node maintains a synchronized topology view through mentorship and piggybacking. ESC simply leverages the GTT as the shared context table, requiring zero additional infrastructure.

### 3.2 Core Concept: Entropy from GTT

**Entropy `k`** = minimum number of trailing address bytes needed to uniquely identify every node in the GTT.

```
k = min { j in [1, addrLen] : suffix(a_i, j) != suffix(a_k, j) for all i != k }
```

Examples:
- **`/24` subnet** (10.1.1.0/24): all addresses differ in the last byte → `k = 1`
- **Multi-subnet** (10.1.1.5 and 10.1.2.5): last byte collides → `k = 2`
- **IPv6**: depends on interface ID diversity

Since the GTT converges across nodes (via mentorship + piggybacking), entropy agreement follows naturally.

### 3.3 Address Mode (AM)

Each address field carries a 3-bit AM telling the receiver how many bytes follow:

| AM | Inline Bytes | Usage |
|----|---|---|
| `111` | 1 byte | Suffix-1: `/24` networks (most IoT) |
| `110` | 2 bytes | Suffix-2: `/16` multi-subnet |
| `101` | 3 bytes | Half MAC (OUI-stripped MAC-24 suffix) |
| `100` | 4 bytes | Full IPv4 address |
| `011` | 6 bytes | Full MAC-48 address |
| `010` | 8 bytes | Half IPv6 (interface ID, prefix elided) |
| `001` | 16 bytes | Full IPv6 address |
| `000` | — | Reserved |

The 3-bit AM makes TAVRN PHY-agnostic: the same encoding handles IPv4, IPv6, and raw MAC addresses. The ns-3 implementation uses `AM=111` (1-byte suffix) since all simulations run on a single `/24` subnet.

### 3.4 Implementation: `CompressedEncoding` Class

**File**: `tavrn-packet.h:46`, `tavrn-packet.cc:20`

```cpp
class CompressedEncoding {
    static uint32_t s_networkPrefix;  // e.g., 0x0A010100 for 10.1.1.0/24
    static bool s_initialized;

    // Address compression (ESC k=1)
    static uint8_t IpToNodeId(Ipv4Address addr);     // addr.Get() & 0xFF
    static Ipv4Address NodeIdToIp(uint8_t id);        // s_networkPrefix | id

    // Field compression (applied uniformly, independent of k)
    static uint8_t EncodeTtlBucket(uint16_t secs);    // min(15, (ttl+19)/20) → 4 bits
    static uint16_t DecodeTtlBucket(uint8_t bucket);   // bucket * 20

    static uint16_t CompressSeqNo(uint32_t seqNo);     // low 16 bits
    static uint32_t ExpandSeqNo(uint16_t compressed);   // zero-extend

    static uint16_t EncodeLifetime(uint32_t ms);        // non-linear: direct below 16383, scaled above
    static uint32_t DecodeLifetime(uint16_t encoded);   // inverse
};
```

**Initialization**: called once in `RoutingProtocol::NotifyInterfaceUp()`:
```cpp
CompressedEncoding::SetNetworkPrefix(iface.GetLocal());
// Stores prefix with low octet zeroed: 10.1.1.5 → 0x0A010100
```

### 3.4.1 Implementation Status: What Exists vs. What Is Specified

The ns-3 implementation provides **fixed `k=1` suffix compression**, which is the correct ESC output for a `/24` network. The simulation environment (single `/24` subnet) means `k=1` is always the correct entropy — so the simulation results are valid.

**Implemented:**
- `CompressedEncoding` class with `IpToNodeId()` / `NodeIdToIp()` (1-byte suffix)
- All 9 header types serialize/deserialize using 1-byte addresses
- Field compression: TTL buckets (4-bit), sequence numbers (16-bit), lifetime (non-linear 16-bit)
- `ERerrHeader` carries AM bits on the wire (hardcoded to `11`)
- `ERerrHeader` has the `A` (ambiguity) flag field (always set to `0`)

**Not yet implemented (specified in TAVRN_v2.md but not needed for `/24` evaluation):**
- `ComputeEntropy()` — scanning the GTT to dynamically determine minimum `k`
- Dynamic AM selection — choosing `11`/`10`/`01`/`00` based on computed `k`
- Collision detection — receiver checking for multiple GTT entries matching a suffix
- RERR-with-A-flag recovery — sender increasing entropy and retrying after ambiguity
- Sticky high-water decay — `k` staying elevated for `10 * GTT_TTL` before decaying
- `AM=00` fallback — using full addresses for nodes not in the sender's GTT

These features are designed for multi-subnet or IPv6 deployments where address suffixes can collide. The architecture supports them (header formats reserve the AM bits, RERR has the A-flag), but the ns-3 evaluation does not exercise them because all scenarios use a single `/24` subnet.

**Key point**: the wire sizes measured in simulation are real — every address really is 1 byte on the wire. This is not a simulation shortcut; it is the correct ESC output for the simulated network topology. A multi-subnet scenario would require implementing the dynamic entropy machinery, but the compressed wire format and its overhead savings are genuine.

### 3.5 Compression Impact on Wire Sizes

| Field | Uncompressed (IPv4) | ESC (k=1) | Savings |
|---|---|---|---|
| Node address | 4 bytes | **1 byte** | 75% |
| Sequence number | 4 bytes | **2 bytes** | 50% |
| Request ID | 4 bytes | **2 bytes** | 50% |
| Lifetime | 4 bytes | **2 bytes** (non-linear) | 50% |
| TTL (metadata) | 2 bytes | **4 bits** (bucket) | 75% |
| Timestamp | 4 bytes | **2 bytes** | 50% |

### 3.6 Collision Handling (Specified, Not Yet Implemented)

The spec defines collision recovery for multi-subnet deployments where suffixes can be ambiguous:

1. Receiver detects multiple GTT entries matching a compressed suffix
2. Receiver sends `E_RERR` with Ambiguity flag `A=1`
3. Conflicting full addresses piggybacked as topology metadata
4. Sender updates GTT, recomputes larger `k`, retries

Entropy uses a sticky high-water mark: once `k` increases, it stays elevated for `10 * GTT_TTL` before decaying. Increasing entropy is cheap (send more bytes); decreasing risks ambiguity.

This machinery is not implemented in the ns-3 module because the single-subnet simulation topology guarantees `k=1` is always unambiguous. The RERR A-flag field is reserved in the wire format for future use.

---

## 4. Wire Formats

All sizes shown for `k=1` (1-byte suffix). Every header is preceded by a 1-byte `TypeHeader`.

### 4.1 Header Summary

| Header | Type ID | Size (bytes) | Category | Purpose |
|---|---|---|---|---|
| `TypeHeader` | — | 1 | Framing | Message type discriminator |
| `TopologyMetadataHeader` | — | 2 + 2N | Piggybacked | GTT entries appended to control/data packets |
| `ERreqHeader` | 1 | 10 | Routing+Topology | Route request |
| `ERrepHeader` | 2 | 8 | Routing+Topology | Route reply |
| `ERerrHeader` | 3 | 2 + 3N | Routing+Topology | Route error (with ESC ambiguity flag) |
| `HelloHeader` | 4 | 4 | Topology | Node announcement / keepalive |
| `SyncOfferHeader` | 5 | 3 | Topology | Mentor offers GTT sync |
| `SyncPullHeader` | 6 | 2 | Topology | Mentee requests GTT page |
| `SyncDataHeader` | 7 | 3 + 7N | Topology | Mentor sends GTT entries |
| `TcUpdateHeader` | 8 | 7 | Topology | Join/leave gossip |
| `ERrepAckHeader` | 9 | 1 | Routing | RREP acknowledgment |

### 4.2 Detailed Wire Layouts

**E_RREQ** (10 bytes) — Route Request:
```
[1B flags+AM] [1B hopCount] [2B requestID] [1B dst] [2B dstSeqNo] [1B origin] [2B originSeqNo]
```
- Flags: bit 5 = gratuitous RREP, bit 4 = dest-only, bit 3 = unknown seqno
- Both addresses ESC-compressed to 1 byte
- requestID and seqNos compressed to 16 bits each

**E_RREP** (8 bytes) — Route Reply:
```
[1B flags+AM] [1B hopCount] [1B dst] [2B dstSeqNo] [1B origin] [2B lifetime]
```
- Lifetime uses non-linear encoding (direct below 16383ms, scaled above)
- ACK-required flag in bit 6

**E_RERR** (2 + 3N bytes) — Route Error:
```
[2B packed: N(1) | AM_d(3) | A(1) | destCount(8) | reserved(3)] [N × (1B dst + 2B seqNo)]
```
- Only header that explicitly carries AM bits on wire (hardcoded to `111` in ns-3)
- `A` flag: set when caused by ESC suffix collision
- 3-bit AM_d field supports all 7 address modes
- Count expanded to 8 bits (max 255)

**HELLO** (4 bytes) — Node Announcement:
```
[1B nodeId] [2B seqNo] [1B flags]
```
- flags bit 0 = `isNew` (triggers mentorship)

**SYNC_OFFER** (3 bytes) — Mentor Offer:
```
[1B mentorId] [1B gttSize] [1B menteeId]
```

**SYNC_PULL** (2 bytes) — Page Request:
```
[1B startIndex] [1B count]
```

**SYNC_DATA** (3 + 7N bytes) — GTT Page:
```
[1B startIndex] [1B totalEntries] [1B entryCount]
[N × (1B nodeId + 2B lastSeen + 1B ttlBucket + 2B seqNo + 1B hopCount)]
```
- Note: spec says SYNC_DATA uses full addresses (`AM=00`) for bootstrap safety, but implementation uses 1-byte suffix (acceptable for `/24` simulation)

**TC_UPDATE** (7 bytes) — Topology Change:
```
[1B originId] [2B seqNo] [1B subjectId] [1B eventType] [2B timestamp]
```
- eventType: 0 = NODE_JOIN, 1 = NODE_LEAVE

**Topology Metadata** (2 + 2N bytes) — Piggybacked GTT entries:
```
[1B count] [1B AM+flags]
[N × (1B nodeId + 1B packed{4-bit ttlBucket | 4-bit flags})]
```
- flags bit 0 = `freshness_request`
- Appended to E_RREQ, E_RREP, E_RERR, and conditionally to data packets

### 4.3 Comparison: TAVRN vs AODV vs OLSR Wire Sizes

| Packet | AODV (RFC 3561) | TAVRN (k=1) | Savings |
|---|---|---|---|
| RREQ | 24 bytes | **10 bytes** | 58% |
| RREP | 20 bytes | **8 bytes** | 60% |
| RERR (1 dest) | 12 bytes | **5 bytes** | 58% |
| HELLO | 20 bytes (= RREP) | **4 bytes** | 80% |

OLSR TC messages carry full 4-byte IPs per MPR selector. With 25 nodes and ~8 selectors per TC, that is 32+ bytes of addresses alone. DSDV routing updates carry `(4B dest + 4B seqno + metric)` per node — a 49-node update is ~590 bytes minimum.

TAVRN carries the same information in drastically fewer bytes because ESC compresses every address to 1 byte, and the GTT (which every node already maintains) serves as the shared decompression context.

---

## 5. Global Topology Table (GTT)

### 5.1 What the GTT Is

The GTT is a passive, local topology knowledge store. It answers **"who exists in the network?"** — distinct from the routing table which answers **"how do I reach them?"**

Every node maintains its own GTT. Entries are populated through:
- Mentorship (bulk import at bootstrap)
- Passive learning (data/control packet forwarding)
- Piggybacked metadata
- TC_UPDATE gossip

The GTT does **not** schedule timers or send packets. The routing protocol owns all active decisions.

### 5.2 Entry Structure

**File**: `tavrn-gtt.h`

```cpp
struct GttEntry {
    Ipv4Address nodeAddr;   // Node's IP address
    Time lastSeen;          // Absolute time of last evidence
    Time ttlExpiry;         // Hard expiry deadline (absolute)
    Time softExpiry;        // Soft expiry deadline (absolute)
    uint32_t seqNo;         // Latest known sequence number
    uint16_t hopCount;      // Estimated hop distance
    bool departed;          // Confirmed gone?
    Time perNodeTtl;        // Per-entry TTL override (0 = use default)
};
```

### 5.3 Dual-Stage Expiry Model

```
        lastSeen              softExpiry              ttlExpiry
           |                      |                       |
    ──────►├──────── ACTIVE ──────┤── SOFT EXPIRED ───────┤── HARD EXPIRED ──►
           |                      |                       |
           |  No action needed    | Piggyback freshness   | Send verification
           |                      | request on next        | or mark departed
           |                      | outgoing packet        |
```

- **Soft expiry** = `lastSeen + jitteredTtl * softExpiryThreshold` (default threshold: 0.5)
- **Hard expiry** = `lastSeen + jitteredTtl`
- **Soft-expired**: on the next outgoing control or data packet, piggyback a freshness request for this node. This is zero-cost topology maintenance.
- **Hard-expired**: actively verify the node is alive (see Section 8.6).

### 5.4 TTL Jitter

**Problem**: without jitter, all GTT entries created around the same time expire simultaneously, causing a verification storm.

**Solution**: `JitteredTtl(baseTtl) = baseTtl + uniform(0, baseTtl/6)`

With default `GttTtl = 300s`, jitter adds 0–50s, spreading expiries across a 50s window. This is injected via an RNG set by the routing protocol at startup.

Applied in: `AddOrUpdateEntry()`, `RefreshEntry()`.
Not applied in: `MergeEntry()` (preserves imported timestamps), `SetPerNodeTtl()`, `ClampAllPerNodeTtls()`.

### 5.5 Key Operations

| Operation | Behavior |
|---|---|
| `AddOrUpdateEntry(addr, seqNo, hop)` | Insert or update if `seqNo >= existing`. Applies jitter. Resurrects departed entries. |
| `RefreshEntry(addr, seqNo)` | Reset TTL timers without changing hop count. Applies jitter. |
| `MarkDeparted(addr)` | Flag as departed. Overwrite `ttlExpiry` with departure time. |
| `Purge()` | Delete departed entries older than `2 * defaultTtl`. |
| `GetSoftExpiredEntries()` | Active entries where `now >= softExpiry && now < ttlExpiry`. |
| `GetHardExpiredEntries()` | Active entries where `now >= ttlExpiry`. |
| `MergeEntry(entry)` | Used during mentorship. Accepts if `seqNo >= local`. Preserves imported timestamps. |
| `GetEntriesPage(start, count)` | Paginated export for SYNC_DATA. Includes departed entries. |
| `NodeExists(addr)` | Returns `true` only if active (not departed). |
| `EnumerateNodes()` | Returns list of all active node addresses. |

### 5.6 Entry Lifecycle

```
  [New node discovered]
         │
         ▼
     ┌────────┐  RefreshEntry / AddOrUpdateEntry
     │ ACTIVE │◄─────────────────────────────────┐
     └────┬───┘                                   │
          │ time passes                           │
          ▼                                       │
   ┌─────────────┐  piggyback freshness request   │
   │ SOFT EXPIRED │──────────────────────────────►│ (response refreshes)
   └──────┬──────┘                                │
          │ more time                              │
          ▼                                       │
   ┌──────────────┐  send verification            │
   │ HARD EXPIRED │───────────────────────────────┘ (if response)
   └──────┬───────┘
          │ verification failed
          ▼
   ┌──────────┐  TC_UPDATE(LEAVE)
   │ DEPARTED │
   └─────┬────┘
         │ after 2 * defaultTtl
         ▼
     [Purged from table]
```

---

## 6. Routing Table

### 6.1 Overview

**File**: `tavrn-rtable.{h,cc}`

Standard AODV-style destination-keyed routing table. Stores next-hop forwarding entries with sequence numbers, hop counts, precursor lists, and validity states.

### 6.2 Entry Structure

```cpp
struct RoutingTableEntry {
    Ipv4Route m_ipv4Route;       // dest, gateway, source, output device
    Ipv4InterfaceAddress m_iface; // output interface
    RouteFlags m_flag;            // VALID | INVALID | IN_SEARCH
    uint32_t m_seqNo;            // destination sequence number
    uint16_t m_hops;             // hop count
    Time m_lifeTime;             // absolute expiry time
    vector<Ipv4Address> m_precursorList;  // upstream nodes using this route
    bool m_blackListState;        // unidirectional link blacklist
    Time m_blackListTimeout;      // blacklist expiry
    uint8_t m_reqCount;           // RREQ retry counter
};
```

### 6.3 Route States

| State | Meaning | Transitions |
|---|---|---|
| `VALID` | Active, usable for forwarding | → `INVALID` on expiry or RERR |
| `INVALID` | Retained temporarily after failure | → deleted after `badLinkLifetime` |
| `IN_SEARCH` | Route discovery in progress | → `VALID` on RREP, → `INVALID` on timeout |

### 6.4 Interaction with GTT

The routing table and GTT are **deliberately decoupled**:
- **Routing table**: "How do I reach node X?" (next hop, interface, hop count)
- **GTT**: "Does node X exist? When was it last seen?"

This decoupling means:
- A node can know X exists (GTT) without having a route to X (routing table)
- A route to X can expire without affecting GTT knowledge that X exists
- Route discovery can consult GTT for hop estimates (Smart TTL) without creating circular dependencies

---

## 7. Neighbor Table

### 7.1 Overview

**File**: `tavrn-neighbor.{h,cc}`

Manages 1-hop neighbor liveness. Populated externally by the routing protocol when packets are received. Entries expire based on absolute timestamps.

### 7.2 Key Features

- **ARP-backed MAC resolution**: resolves IP → MAC via registered ARP caches
- **TX error detection**: WiFi MAC retry-limit failures trigger immediate neighbor removal
- **RSSI cache**: stores per-neighbor signal strength (used for mentorship backoff)
- **Link-failure callback**: fires on purge, allowing the routing protocol to trigger RERR

### 7.3 Neighbor Lifecycle

```
  [Packet received from X]
         │
         ▼
  Update(X, timeout)  ← extends expiry to max(existing, now + timeout)
         │
         ├── normal operation: neighbor stays alive through periodic traffic
         │
         ├── TX error (MAC retry limit) → mark close=true → immediate Purge
         │                                  → link-failure callback
         │                                  → RERR generated
         │
         └── expiry reached → Purge removes entry → link-failure callback
```

---

## 8. Routing Protocol Core

**File**: `tavrn-routing-protocol.{h,cc}` (~4500 lines)

This is the orchestrator. It owns all timers, makes all transmission decisions, and coordinates the GTT, routing table, and neighbor table.

### 8.1 Initialization and Startup

1. **`DoInitialize()`**: computes derived timers, configures GTT/queue settings, arms HELLO timer
2. **`Start()`**: wires neighbor link-failure callback, starts RREQ/RERR rate limiters, starts GTT maintenance timer, arms 3s bootstrap timeout
3. **`NotifyInterfaceUp()`**: creates UDP sockets (unicast + broadcast) on port 655, initializes ESC with network prefix, adds self to GTT, schedules initial HELLO with 0–100ms jitter

### 8.2 Passive Learning (The "Free" Topology Maintenance)

Every received packet updates the GTT. This is TAVRN's core advantage — topology knowledge is maintained as a side effect of normal traffic.

**In `RouteInput()` (data packets)**:
- Strip piggybacked `TopologyMetadataHeader` if present
- Refresh GTT entry for `prevHop` (1-hop alive)
- Refresh GTT entry for `origin` (IP source is alive)
- Extend route lifetimes for both

**In `Forwarding()` (transit traffic)**:
- Refresh GTT entries for both `origin` and `dst`
- This is the key mechanism: forwarding traffic from A→B through node C proves to C that both A and B are alive
- Conditionally piggyback metadata if soft-expired entries exist

**In `RecvRequest()` (RREQ)**:
- Update GTT for `origin` with hop count from RREQ
- Refresh GTT for `sender` as 1-hop neighbor
- Process piggybacked topology metadata

**In `RecvReply()` (RREP)**:
- Update GTT for `dst` with hop count from RREP
- Refresh GTT for `sender` as 1-hop neighbor
- Process piggybacked topology metadata

**In `RecvHello()`**:
- Update GTT for sender with `hop=1`
- Process piggybacked freshness metadata

**In `RecvTcUpdate()`**:
- NODE_JOIN: add/refresh subject in GTT
- NODE_LEAVE: mark subject as departed

### 8.3 Route Discovery (Enhanced AODV)

TAVRN retains AODV's core route discovery but enhances it:

**Smart TTL**: if the destination is in the GTT with a fresh hop estimate:
- Initial RREQ TTL = `gttHopCount + 2` (instead of starting Expanding Ring Search from TTL=1)
- If it fails, next retry falls back to full flood (`NetDiameter`)
- This avoids wasting broadcast waves when the protocol already knows the distance

**Unicast Intercept**: while forwarding an RREQ, if the destination is a fresh 1-hop neighbor:
- Unicast the RREQ directly instead of rebroadcasting
- Saves one broadcast hop in dense networks

**Expanding Ring Search (when no GTT info available)**:
- TTL progression: 1 → 3 → 5 → 7 → 35 (NetDiameter)
- `TtlStart=1`, `TtlIncrement=2`, `TtlThreshold=7`

**Rate limiting**: max 10 RREQ/s and 10 RERR/s.

### 8.4 Piggybacking

TAVRN has two piggybacking modes:

**Mode A — Clean forwarding** (stable topology):
- No soft-expired GTT entries
- Data packets forwarded without any TAVRN header
- Zero overhead per data packet

**Mode B — Conditional metadata** (refresh needed):
- Soft-expired GTT entries exist
- `TopologyMetadataHeader` prepended to data packet
- `TavrnShimTag` attached (carries real previous-hop IP, since IP source is the flow origin)
- Next hop strips metadata, processes it, forwards clean payload onward

Control packets (E_RREQ, E_RREP, E_RERR) **always** carry topology metadata.

**Why the shim tag?** On a multi-hop data flow A→B→C→D, when C receives from B, the IP source says A (the flow origin). But the piggybacked metadata was attached by B. The shim tag tells C that B is the piggybacker, so C can properly attribute 1-hop freshness.

### 8.5 Mentorship (Bootstrap)

How a new node gets its initial GTT:

```
New Node                    Neighbor (bootstrapped)
   │                               │
   │──── HELLO (isNew=true) ──────►│
   │                               │ compute RSSI-based backoff
   │                               │ (strong signal = shorter delay)
   │◄──── SYNC_OFFER ─────────────│
   │  (mentorId, gttSize, menteeId)│
   │                               │
   │  [collect offers for 2s]      │
   │  [pick mentor with largest GTT]
   │                               │
   │──── SYNC_PULL(0, 15) ────────►│
   │◄──── SYNC_DATA(0, N, entries) │
   │──── SYNC_PULL(15, 15) ───────►│
   │◄──── SYNC_DATA(15, N, entries)│
   │  ... repeat until all pages ...│
   │                               │
   │  [mark self as bootstrapped]  │
   │  [broadcast TC_UPDATE(JOIN)]  │
```

**RSSI-based mentor selection**: stronger signal → shorter backoff → first offer arrives from the best link. Mapping: `-90dBm → 500ms`, `-30dBm → 10ms`, linear interpolation + 0–50ms jitter.

**Mentor failure recovery**: each SYNC_PULL has a timeout of `2 * NetTraversalTime`. After 3 timeouts, the node clears its mentor, rebroadcasts HELLO, and restarts bootstrap.

**Self-bootstrap**: if no offer arrives within 3s, the node self-bootstraps with only itself in the GTT and announces JOIN via TC_UPDATE.

### 8.6 GTT Maintenance and Verification

**Timer**: `GttMaintenanceTimerExpire()` runs every `max(1s, GttTtl/12)` — default every 25s.

Each cycle:
1. Refresh self in GTT
2. Call `CheckGttExpiry()` for hard-expired entries
3. Call `CheckNeighborLiveness()` for silent neighbor departures

**`CheckGttExpiry()` — the verification FSM**:

```
[Hard-expired entry]
        │
        ├─ Is there queued traffic or active route using this node?
        │     NO → silently mark departed (no demand = no verification)
        │     YES ↓
        │
        ├─ Stage 0: try cheap unicast verification via existing route
        │     Success → refresh GTT, done
        │     Fail/timeout ↓
        │
        ├─ Stage 1: send RREQ (smart TTL, then ERS if needed)
        │     Success → refresh GTT, done
        │     Fail ↓
        │
        └─ Mark departed, broadcast TC_UPDATE(NODE_LEAVE)
```

**Verification cap**: max 3 new verifications per maintenance cycle. This prevents storms when many entries expire simultaneously (even with jitter).

**Demand-gated verification**: nodes only actively verify entries they're actually using. An off-path node with an expired entry for a distant leaf node will silently mark it departed — no wasted bandwidth.

### 8.7 Adaptive HELLO

HELLO interval dynamically adjusts between `HelloIntervalMin` (24s) and `HelloIntervalMax` (120s):

- **EMA growth**: `interval_new = alpha * interval_old + (1 - alpha) * interval_max` (alpha=0.8)
- **Reset trigger**: neighbor count changes (gain or loss) → immediate reset to minimum
- **Suppression**: if any other broadcast was recently sent, skip this HELLO cycle

### 8.8 TC_UPDATE (Topology Change Gossip)

Broadcast on confirmed join/leave events:
- Mentorship completion → `TC_UPDATE(NODE_JOIN)`
- Verification failure → `TC_UPDATE(NODE_LEAVE)`
- MAC retry-limit failure → `TC_UPDATE(NODE_LEAVE)`
- Neighbor liveness timeout → `TC_UPDATE(NODE_LEAVE)`

**Forwarding**: TTL starts at `NetDiameter`, decremented on each hop. Not re-flooded from net diameter — bounded propagation.

**Deduplication**: two layers:
1. UUID-based: `(originator_ip << 32 | seqno)` with 30s cache
2. Subject-based: `{subject, event}` with 1s window (prevents amplification when multiple neighbors independently detect the same departure)

### 8.9 Link Failure Detection

Two detection mechanisms:

1. **MAC retry-limit** (`NotifyTxError`): WiFi reports `REACHED_RETRY_LIMIT` → resolve MAC→IP → mark neighbor departed → RERR → TC_UPDATE(LEAVE)

2. **Silent death** (`CheckNeighborLiveness`): if `now - lastSeen > max(3 * helloInterval, livenessFloor)` → mark departed → TC_UPDATE(LEAVE). The `livenessFloor` decays gradually to prevent false positives when HELLO interval resets from slow to fast mode.

---

## 9. Lifecycle Walkthroughs

### 9.1 New Node Joins the Network

1. Node boots, `NotifyInterfaceUp()` initializes ESC, adds self to GTT
2. Broadcasts HELLO with `isNew=true`
3. Bootstrapped neighbors compute RSSI backoff, strongest sends SYNC_OFFER
4. Node collects offers for 2s, picks mentor with largest GTT
5. Sends paginated SYNC_PULL requests, receives SYNC_DATA pages
6. Each page is merged into GTT via `MergeEntry()`
7. Node marks itself bootstrapped, broadcasts TC_UPDATE(NODE_JOIN)
8. All other nodes update their GTT with the new node
9. Node begins participating in data forwarding and passive learning

### 9.2 Data Packet Traverses the Network

```
App on Node A sends to Node D:

A: RouteOutput() → has valid route → forward
   Forwarding():
     - Refresh GTT(origin=A, dst=D)
     - Soft-expired entries exist? → attach TopologyMetadataHeader + ShimTag
     - Send to next-hop B

B: RouteInput():
     - Strip metadata header, process piggybacked entries
     - Refresh GTT(prevHop=A) at hop=1, GTT(origin=A)
   Forwarding():
     - Refresh GTT(origin=A, dst=D)
     - Conditionally piggyback more metadata
     - Send to next-hop C

C: RouteInput():
     - Same processing
   Forwarding():
     - Refresh GTT for A and D
     - Forward to D

D: RouteInput():
     - Strip metadata, process it
     - Refresh GTT(prevHop=C, origin=A)
     - Deliver to application
```

Result: nodes A, B, C, D all refreshed GTT entries for each other — for free, as a side effect of data delivery. No dedicated control messages sent.

### 9.3 Node Silently Fails

1. Node X stops responding (hardware failure, battery death)
2. HELLO timer expires: neighbors notice `lastSeen` too old in `CheckNeighborLiveness()`
3. Neighbors mark X departed in their GTTs, broadcast TC_UPDATE(NODE_LEAVE)
4. TC_UPDATE propagates through the network (TTL-bounded flooding)
5. Each receiver updates its GTT: X is departed
6. After `2 * GttTtl`, departed entry is purged from all GTTs

If node X was an active flow endpoint:
1. `CheckGttExpiry()` detects hard-expired entry with queued traffic
2. Verification stage 0: unicast check fails (no response)
3. Verification stage 1: RREQ fails (no RREP)
4. Mark departed, TC_UPDATE(NODE_LEAVE), RERR to precursors

---

## 10. Key Constants and Tunable Parameters

### 10.1 Primary Parameters

| Parameter | Default | Description |
|---|---|---|
| `GttTtl` | 300s | GTT entry time-to-live |
| `SoftExpiryThreshold` | 0.5 | Fraction of TTL for soft expiry (150s default) |
| `HelloIntervalMin` | 24s | Minimum adaptive HELLO interval |
| `HelloIntervalMax` | 120s | Maximum adaptive HELLO interval |
| `HelloAlpha` | 0.8 | EMA smoothing for HELLO interval growth |
| `MaxMetadataEntries` | 5 | Max piggybacked GTT entries per packet |
| `PiggybackCooldown` | 5s | Per-entry cooldown before re-piggybacking |
| `NetDiameter` | 35 | Max network diameter in hops |
| `NodeTraversalTime` | 40ms | One-hop traversal time estimate |
| `RreqRetries` | 2 | Max RREQ retry count |

### 10.2 Derived Timers

| Timer | Formula | Default |
|---|---|---|
| `ActiveRouteTimeout` | `1.2 * GttTtl` | 360s |
| `NetTraversalTime` | `2 * NetDiameter * NodeTraversalTime` | 2.8s |
| `PathDiscoveryTime` | `2 * NetTraversalTime` | 5.6s |
| `GttMaintenanceInterval` | `max(1s, GttTtl / 12)` | 25s |
| `BlackListTimeout` | `RreqRetries * NetTraversalTime` | 5.6s |

### 10.3 Internal Constants

| Constant | Value | Where |
|---|---|---|
| `kMaxNewVerificationsPerCycle` | 3 | `CheckGttExpiry()` |
| `TTL jitter range` | `uniform(0, baseTtl/6)` | `JitteredTtl()` |
| `TtlStart` (ERS) | 1 | `SendRequest()` |
| `TtlIncrement` (ERS) | 2 | `SendRequest()` |
| `TtlThreshold` (ERS) | 7 | `SendRequest()` |
| `RREQ rate limit` | 10/s | `SendRequest()` |
| `Bootstrap timeout` | 3s | `Start()` |
| `Offer collection window` | 2s | `RecvSyncOffer()` |
| `SYNC page size` | 15 entries | `SendSyncPull()` |
| `Mentor retry limit` | 3 | `SyncPullTimeout()` |
| `TC_UPDATE UUID cache` | 30s | `RecvTcUpdate()` |
| `TC_UPDATE subject dedup` | 1s | `RecvTcUpdate()` |
| `Departed entry retention` | `2 * GttTtl` | `Purge()` |

---

## 11. Design Decisions and Trade-offs

### 11.1 Why Piggybacking Instead of Periodic Flooding

OLSR floods TC messages every 5s. DSDV floods routing table updates periodically. At 50 nodes, this costs 10,000+ B/s of steady-state overhead.

TAVRN's approach: if data is already flowing (which it is in any active IoT deployment), piggyback topology metadata on those packets. Cost: 1 + 2N bytes per piggybacked packet (typically 11 bytes for 5 entries). If no data is flowing, the soft-expiry → hard-expiry → verification pipeline activates — but only for nodes you're actually communicating with.

**Trade-off**: TAVRN's topology knowledge depends on traffic patterns. In a hub-spoke smart home, the hub's traffic keeps everyone's GTT fresh. In a flat grid with random flows, off-path nodes lose knowledge of each other. This is the root cause of the ~0.92 GTT recall at 50 nodes in grid scenarios.

### 11.2 Why ESC Instead of Fixed Short Addresses

Fixed short addresses (Zigbee, Thread, Z-Wave) require a coordinator to assign them. ESC derives compression from the GTT — which already exists. No coordinator, no assignment protocol, no single point of failure.

**Trade-off**: ESC requires GTT convergence for correct decompression. During bootstrap or after network partition, nodes may have divergent GTTs and compute different entropy values. The collision handling mechanism (RERR with A-flag) recovers from this, but it adds latency to the first packet after divergence.

### 11.3 Why Demand-Gated Verification

Hard-expired GTT entries are only actively verified if there's queued traffic or an active route using that node. This prevents the protocol from wasting bandwidth verifying nodes nobody is talking to.

**Trade-off**: off-path nodes lose GTT entries for distant leaf nodes. The network-wide topology recall drops from 1.00 to ~0.92 at 50 nodes. The hub node itself maintains ~1.00 recall because all traffic flows through it.

### 11.4 Why Mentorship Instead of Network-Wide Bootstrap

OLSR/DSDV: a new node learns the topology by listening to periodic floods — can take minutes. TAVRN: a neighbor ships its full GTT in paginated SYNC_DATA messages — takes seconds.

**Trade-off**: mentorship quality depends on the mentor's GTT. If the mentor has a stale or incomplete GTT, the mentee inherits those gaps. The RSSI-based mentor selection and largest-GTT-wins policy mitigate this.

### 11.5 Why Adaptive HELLO Instead of Fixed Interval

Fixed HELLO (AODV default: 1s) wastes bandwidth in stable networks. No HELLO means slow neighbor-death detection. Adaptive HELLO starts fast (24s) after topology changes, then decays to slow (120s) during stability.

**Trade-off**: the decay means neighbor failures in stable networks take longer to detect (up to `3 * 120s = 360s` worst case). MAC retry-limit detection is much faster for active flows, so this primarily affects idle neighbors.

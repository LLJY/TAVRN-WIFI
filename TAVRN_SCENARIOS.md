# TAVRN v2.1 Evaluation Scenarios

Full scenario catalog for the TAVRN ns-3 simulation harness (`scratch/tavrn-comparison.cc`).
Sweep runner: `scratch/run-all-scenarios.sh`.

## Common Defaults

| Parameter | Default | Notes |
|-----------|---------|-------|
| `simTime` | 600s (sweep), 300s (smoke) | Configurable via CLI |
| `nRuns` | 5 (sweep) | 95% CI computed across seeds |
| `packetSize` | 512 bytes | UDP payload |
| `dataRate` | 4 pps per flow | Unless overridden |
| `appPort` | 9 | Base UDP port |
| `appStart` | 60s | Warmup before data flows begin |
| Protocols | TAVRN, AODV, OLSR, DSDV | All four compared |

## Metrics Collected

| Metric | Key | Description |
|--------|-----|-------------|
| PDR | `pdr_mean` | Packet Delivery Ratio (0-1) |
| Latency | `latency_mean` | Average end-to-end latency (ms) |
| Steady Overhead | `steadyBps_mean` | PHY TX bytes/sec during steady state (after 60s warmup) |
| Bootstrap Overhead | `bootstrapBytes_mean` | Total PHY TX bytes during first 60s |
| TX Energy | `txEnergy_mean` | Radio TX energy (Joules) |
| RX Energy | `rxEnergy_mean` | Radio RX energy (Joules) |
| GTT Accuracy | `gttAccuracy_mean` | Topology knowledge vs ground truth (TAVRN+OLSR only) |
| Convergence | `convergence_mean` | GTT convergence time (ms, TAVRN only) |
| Route Discovery | `routeDiscovery_mean` | Route discovery latency (ms, TAVRN only) |

---

## A. Traffic Load Scenarios

### sensor-dense

Tests high-traffic IoT workload (many sensors reporting frequently).

| Parameter | Value |
|-----------|-------|
| Scenario | `grid` |
| Nodes | 25 (5x5 grid) |
| Area | 500x500m |
| Flows | 10 |
| Data Rate | 20 pps |
| Mobility | Static |

**What it stresses**: Channel contention, MAC retries, control overhead under load. High flow count forces many concurrent route discoveries. 20 pps is aggressive for IoT.

### sensor-light

Tests minimal IoT workload (few sensors, infrequent reports).

| Parameter | Value |
|-----------|-------|
| Scenario | `grid` |
| Nodes | 25 (5x5 grid) |
| Area | 500x500m |
| Flows | 2 |
| Data Rate | 1 pps |
| Mobility | Static |

**What it stresses**: Idle overhead. With very little data traffic, this isolates the protocol's background maintenance cost. TAVRN should show near-zero overhead here due to conditional piggybacking; OLSR still sends periodic HELLOs/TCs.

---

## B. Network Scale Scenarios

### small

Minimal network for baseline behavior.

| Parameter | Value |
|-----------|-------|
| Scenario | `grid` |
| Nodes | 9 (3x3 grid) |
| Area | 300x300m |
| Flows | 3 |
| Mobility | Static |

**What it stresses**: Small-network baseline. All protocols should perform well here. Tests that TAVRN doesn't over-engineer for tiny networks.

### medium

Standard mid-size network (primary benchmark).

| Parameter | Value |
|-----------|-------|
| Scenario | `grid` |
| Nodes | 25 (5x5 grid) |
| Area | 500x500m |
| Flows | 5 |
| Mobility | Static |

**What it stresses**: Balanced test. Most directly comparable to academic MANET papers that commonly use 20-30 nodes.

### large

Larger network approaching academic scalability benchmarks.

| Parameter | Value |
|-----------|-------|
| Scenario | `grid` |
| Nodes | 49 (7x7 grid) |
| Area | 700x700m |
| Flows | 10 |
| Mobility | Static |

**What it stresses**: Multi-hop routing, overhead scaling. OLSR's O(n^2) TC overhead becomes visible. TAVRN's piggybacking should scale better. 49 nodes is upper range for our current benchmarks.

---

## C. Mobility Scenarios

### mobile-slow

Pedestrian-speed mobility (walking in a building).

| Parameter | Value |
|-----------|-------|
| Scenario | `mobile` (RandomWaypoint) |
| Nodes | 25 |
| Area | 500x500m |
| Max Speed | 1.0 m/s |
| Pause Time | 5.0s |
| Flows | 5 |

**What it stresses**: Gentle topology changes. Routes break occasionally, requiring rediscovery. Tests TAVRN's passive topology learning and route maintenance without aggressive churn.

### mobile-fast

High-speed mobility (running/vehicles, continuous movement).

| Parameter | Value |
|-----------|-------|
| Scenario | `mobile` (RandomWaypoint) |
| Nodes | 25 |
| Area | 500x500m |
| Max Speed | 5.0 m/s |
| Pause Time | 0.0s |
| Flows | 5 |

**What it stresses**: Frequent route breaks, RERR storms, route rediscovery overhead. This is where reactive protocols generate the most control traffic. Tests TAVRN's NotifyTxError (C3 fix) and Smart TTL retry logic.

---

## D. Disruption / Churn Scenarios

### node-failure

Sudden node failures mid-simulation.

| Parameter | Value |
|-----------|-------|
| Scenario | `grid` |
| Nodes | 25 |
| Area | 500x500m |
| Flows | 5 |
| Failed Nodes | 5 (20% of network) |
| Fail Time | 300s |
| Mobility | Static |

**What it stresses**: Route repair after sudden topology loss. 5 nodes go down at t=300s, potentially breaking active routes. Tests RERR propagation, rerouting speed, and GTT `MarkDeparted()`.

### node-rejoin

Node failure followed by recovery.

| Parameter | Value |
|-----------|-------|
| Scenario | `grid` |
| Nodes | 25 |
| Area | 500x500m |
| Flows | 5 |
| Failed Nodes | 5 |
| Fail Time | 300s |
| Rejoin Time | 450s |
| Mobility | Static |

**What it stresses**: Recovery after disruption. At t=450s, failed nodes come back. Tests GTT resurrection logic (RefreshEntry clearing departed flag) and route re-establishment.

### mobile-churn

Rolling churn with fast mobility (transient nodes entering/leaving).

| Parameter | Value |
|-----------|-------|
| Scenario | `mobile` (RandomWaypoint) |
| Nodes | 25 |
| Area | 500x500m |
| Max Speed | 5.0 m/s |
| Pause Time | 0.0s |
| Flows | 5 |
| Churn Nodes | 5 per rotation |
| Churn Interval | 60s |

**What it stresses**: Combined mobility + churn. Every 60s, 5 random non-endpoint nodes go down and the previous batch comes back up. Starts at t=120s (after bootstrap). Simulates a transient environment like vehicles or people passing through. Everyone comes back at simTime-30s for final measurement.

---

## E. Topology Stress Scenarios

### sparse

Stretched network with potential partitioning.

| Parameter | Value |
|-----------|-------|
| Scenario | `grid` |
| Nodes | 25 |
| Area | 1000x1000m |
| Flows | 5 |
| Mobility | Static |

**What it stresses**: Long multi-hop paths, possible network partitions. 200m spacing between grid nodes may exceed WiFi range for some pairs. Tests route discovery over many hops and RREQ TTL settings.

### linear

Chain topology forcing maximum hop count.

| Parameter | Value |
|-----------|-------|
| Scenario | `linear` (chain) |
| Nodes | 10 |
| Area | 500m (line length) |
| Flows | 1 (end-to-end) |
| Mobility | Static |

**What it stresses**: Pure multi-hop performance. Single flow from node 0 to node 9 traversing 9 hops. Tests per-hop latency accumulation, piggybacking along the chain, and Smart TTL accuracy (GTT knows the exact hop count).

---

## F. Bursty Traffic Scenario

### bursty

Intermittent on/off traffic pattern (IoT duty-cycling).

| Parameter | Value |
|-----------|-------|
| Scenario | `grid` |
| Nodes | 25 |
| Area | 500x500m |
| Flows | 5 |
| On Time | 5s |
| Off Time | 15s |
| Mobility | Static |

**What it stresses**: Route aging during off periods. With 15s off periods, routes may expire and need rediscovery when traffic resumes. Tests TAVRN's conditional piggybacking (metadata during off periods keeps topology fresh even without data).

---

## G. Smart Home Scenarios

All smart home scenarios use:
- **`home` topology**: Procedurally generated room-corridor layout. Nodes clustered in rooms (~30m radius), rooms spaced ~100m apart (WiFi range boundary), 1-2 corridor nodes bridging adjacent rooms. Node 0 is the hub/gateway at the center.
- **`hubTraffic=true`**: Mixed traffic pattern. First half of flows are many-to-one (sensors to hub/node 0). Second half are peer-to-peer (e.g., switch to light).
- **Low data rate**: 1 pps (realistic IoT sensor reporting).
- **Static placement**: Devices in a home don't move.

#### Room Count Scaling

| Nodes | Rooms | Devices/Room | Corridor Nodes | Real-World Analogy |
|-------|-------|--------------|----------------|-------------------|
| 15 | 3 | ~4 | 2-3 | Small apartment |
| 30 | 6 | ~4 | 5-7 | Average house |
| 50 | 10 | ~4 | 9-12 | Smart home enthusiast |

### home-small

Small apartment deployment.

| Parameter | Value |
|-----------|-------|
| Scenario | `home` |
| Nodes | 15 |
| Flows | 5 (3 hub-bound, 2 peer-to-peer) |
| Data Rate | 1 pps |
| Hub Traffic | Yes |
| Rooms | ~3 |
| Mobility | Static |

**What it stresses**: Baseline smart home performance. Small, well-connected network. All protocols should deliver high PDR. Tests that TAVRN's overhead advantage holds even in small networks.

### home-medium

Average house deployment.

| Parameter | Value |
|-----------|-------|
| Scenario | `home` |
| Nodes | 30 |
| Flows | 8 (5 hub-bound, 3 peer-to-peer) |
| Data Rate | 1 pps |
| Hub Traffic | Yes |
| Rooms | ~6 |
| Mobility | Static |

**What it stresses**: Realistic mid-size home. Room-corridor topology creates 2-3 hop paths between distant rooms. Hub-centric traffic tests route convergence at node 0. OLSR's periodic flooding becomes expensive relative to low data rate.

### home-large

Smart home enthusiast deployment.

| Parameter | Value |
|-----------|-------|
| Scenario | `home` |
| Nodes | 50 |
| Flows | 12 (7 hub-bound, 5 peer-to-peer) |
| Data Rate | 1 pps |
| Hub Traffic | Yes |
| Rooms | ~10 |
| Mobility | Static |

**What it stresses**: Large-scale home with many rooms and chokepoint corridors. Multi-hop paths through corridor nodes. TAVRN's piggybacking on the low-rate traffic should keep overhead minimal. OLSR's TC flooding across 50 nodes generates significant background traffic relative to the 12 low-rate flows.

### home-hell

Stress test: large home with device churn.

| Parameter | Value |
|-----------|-------|
| Scenario | `home` |
| Nodes | 50 |
| Flows | 12 (7 hub-bound, 5 peer-to-peer) |
| Data Rate | 1 pps |
| Hub Traffic | Yes |
| Rooms | ~10 |
| Churn Nodes | 1 per rotation |
| Churn Interval | 60s |
| Mobility | Static |

**What it stresses**: Realistic smart home failure mode. 1 random non-endpoint device cycles offline every 60s (battery death, firmware OTA, WiFi disconnect). Tests topology adaptation, GTT accuracy under churn, and overhead stability. Churn starts at t=120s, stops at simTime-60s, all nodes back up at simTime-30s.

---

## Academic Comparison Notes

### How Our Scenarios Map to Literature

| Academic Standard | Our Equivalent | Gap? |
|---|---|---|
| 20-30 node grid (common MANET paper) | `medium` (25 nodes) | Covered |
| 50-100 node scalability test | `large` (49), `home-large` (50) | Partial (no 100-node) |
| RandomWaypoint mobility sweep | `mobile-slow`, `mobile-fast` | Covered (2 points, not full sweep) |
| Node failure/recovery | `node-failure`, `node-rejoin` | Covered |
| Linear/chain multi-hop | `linear` (10 nodes) | Covered |
| IoT sensor traffic patterns | `sensor-light`, `bursty`, `home-*` | Well covered |
| Smart home room topology | `home-*` (4 scenarios) | Covered |
| Many-to-one traffic (sensors to gateway) | `home-*` with `hubTraffic` | Covered |
| 100+ node scalability | None | Known gap |
| TCP traffic | None | Minor gap (IoT typically uses UDP) |
| WiFi interference / co-channel | None | Minor gap |

### Design Decisions

- **WiFi+IPv4 for simulation**: TAVRN targets BLE mesh, but ns-3 WiFi is used for fair comparison against AODV/OLSR/DSDV which have mature ns-3 implementations. This is standard practice in MANET literature.
- **`steadyBps` includes forwarded data**: Our overhead metric is PHY TX bytes minus source app bytes. This includes MAC retries and forwarded data, not just pure control. This slightly disadvantages TAVRN (which forwards data like all protocols) but gives a holistic "channel cost" view.
- **Deterministic seeds**: All scenarios use deterministic seeding for reproducibility. The RNG seed is `seedStart + runIndex`.

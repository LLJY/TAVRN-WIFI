# TAVRN v2.1 — ns-3 WiFi Simulation

**Topology Aware Vicinity-Reactive Network (TAVRN)** is a hybrid mesh routing protocol designed for smart home and IoT environments. It combines reactive route discovery (AODV-derived) with proactive topology awareness (Global Topology Table) and conditional control-plane piggybacking to minimize overhead while maintaining high delivery rates.

This repository contains the full ns-3 module implementation and benchmark harness used to evaluate TAVRN against AODV, OLSR, and DSDV across 17 scenarios.

## Repository Structure

```
TAVRN-WiFi/
├── src/tavrn/              # ns-3 module (drop into ns-3-dev/src/)
│   ├── model/              # Protocol core: routing, GTT, neighbors, packets
│   ├── helper/             # ns-3 helper class
│   ├── test/               # 21 unit tests
│   ├── examples/           # Basic usage example
│   └── CMakeLists.txt      # Build configuration
├── scratch/
│   ├── tavrn-comparison.cc # Benchmark harness (TAVRN vs AODV/OLSR/DSDV)
│   └── run-all-scenarios.sh # Full 17-scenario sweep runner
├── results/                # Sweep output CSVs
├── TAVRN_v2.md             # Protocol specification (authoritative)
├── TAVRN_SCENARIOS.md      # Scenario catalog and rationale
└── README.md
```

## Prerequisites

- **ns-3.45** built from source ([ns-3 installation guide](https://www.nsnam.org/docs/release/3.45/tutorial/html/getting-started.html))
- C++17 compiler (GCC 11+ or Clang 14+)
- CMake 3.16+
- Python 3.8+ (for `./ns3` and `./test.py`)

## Setup

1. **Clone this repo** alongside your ns-3 installation:

```bash
cd /path/to/ns-3-dev
git clone https://github.com/<user>/TAVRN-WiFi.git ../TAVRN-WiFi
```

2. **Symlink the module** into ns-3's source tree:

```bash
# From ns-3-dev root
ln -s /absolute/path/to/TAVRN-WiFi/src/tavrn src/tavrn
ln -s /absolute/path/to/TAVRN-WiFi/scratch/tavrn-comparison.cc scratch/tavrn-comparison.cc
ln -s /absolute/path/to/TAVRN-WiFi/scratch/run-all-scenarios.sh scratch/run-all-scenarios.sh
```

3. **Reconfigure and build**:

```bash
./ns3 configure --enable-examples --enable-tests
./ns3 build tavrn
```

4. **Run tests**:

```bash
./test.py --suite=tavrn
# Expected: 21/21 pass
```

## Running Simulations

### Single Scenario

```bash
./ns3 run "tavrn-comparison --protocol=TAVRN --scenario=grid --nNodes=25 --nFlows=5 --simTime=600 --nRuns=5"
```

Output is a single CSV line to stdout with columns:
```
protocol, scenario, nNodes, nFlows, simTime, nRuns,
pdr_mean, pdr_ci95, latency_mean, latency_ci95,
steadyBps_mean, steadyBps_ci95, bootstrapBytes_mean, bootstrapBytes_ci95,
totalOverhead_mean, totalOverhead_ci95, energy_mean, energy_ci95,
txEnergy_mean, txEnergy_ci95, rxEnergy_mean, rxEnergy_ci95,
gttAccuracy_mean, gttAccuracy_ci95, convergence_mean, convergence_ci95,
routeDiscovery_mean, routeDiscovery_ci95
```

### Full Sweep (17 Scenarios x 4 Protocols)

```bash
bash scratch/run-all-scenarios.sh [nRuns] [simTime] [maxJobs] [--timeout=SECS]
# Defaults: nRuns=5, simTime=600, maxJobs=24, timeout=3600
```

This runs 68 simulation jobs in parallel (throttled to `maxJobs`). Output goes to `results/tavrn-sweep-<timestamp>.csv`.

Jobs exceeding the per-job timeout (default: 1 hour) are killed and marked **DNF** in the CSV (`-1` for all metrics). This is expected for protocols that generate pathological traffic on large topologies (e.g., AODV RREQ flooding on 49-50 node networks).

**Resource requirements**: Large AODV scenarios (49-50 nodes) can use 4-5 GB RAM each. On a 32 GB machine, limit `maxJobs` to 6-8. A full sweep takes 1-4 hours depending on hardware.

### Baseline Mode (TAVRN-only iteration)

If you're iterating on TAVRN and don't need to re-run baseline protocols:

```bash
bash scratch/run-all-scenarios.sh 5 600 8 --baseline=results/previous-sweep.csv
```

This only runs TAVRN and merges AODV/OLSR/DSDV rows from the baseline CSV, saving ~75% of simulation time.

## Scenarios

| Label | Topology | Nodes | Description |
|-------|----------|-------|-------------|
| sensor-dense | Grid | 25 | High traffic (20 kbps, 10 flows) |
| sensor-light | Grid | 25 | Low traffic (1 kbps, 2 flows) |
| small | Grid | 9 | Minimal network |
| medium | Grid | 25 | Baseline grid |
| large | Grid | 49 | Large grid |
| mobile-slow | Mobile | 25 | Random waypoint, 1 m/s |
| mobile-fast | Mobile | 25 | Random waypoint, 5 m/s, no pause |
| node-failure | Grid | 25 | 5 nodes fail at t=300s |
| node-rejoin | Grid | 25 | 5 nodes fail at t=300s, rejoin at t=450s |
| sparse | Grid | 25 | Wide spacing (1000m area) |
| linear | Linear | 10 | Chain topology |
| bursty | Grid | 25 | On/off traffic (5s on, 15s off) |
| mobile-churn | Mobile | 25 | Fast mobile + rolling churn |
| home-small | Smart Home | 15 | 3 rooms, hub traffic |
| home-medium | Smart Home | 30 | 6 rooms, hub traffic |
| home-large | Smart Home | 50 | 10 rooms, hub traffic |
| home-hell | Smart Home | 50 | 10 rooms, hub traffic + device churn |

See [TAVRN_SCENARIOS.md](TAVRN_SCENARIOS.md) for detailed rationale and topology descriptions.

## Protocol Overview

TAVRN combines three mechanisms:

1. **Reactive Routing** (AODV-derived): Route discovery via RREQ/RREP with Smart TTL (GTT-assisted initial TTL estimation).

2. **Global Topology Table (GTT)**: Proactive topology awareness maintained via SYNC protocol (mentor-mentee). Each node builds a full network view without O(n^2) periodic flooding. Used for TTL estimation, unicast intercept, and topology-aware decisions — **decoupled from routing**.

3. **Conditional Piggybacking** (v2.1): Control information (HELLO, TC-UPDATE) is piggybacked on existing data traffic when available (Mode A), falling back to dedicated packets only when traffic is insufficient (Mode B). A round-robin cursor ensures fair distribution across neighbors.

See [TAVRN_v2.md](TAVRN_v2.md) for the full protocol specification.

## Key Results

Across 17 scenarios comparing TAVRN, AODV, OLSR, and DSDV:

- **Overhead**: TAVRN achieves lowest steady-state overhead in 11/17 scenarios, 2.7-4.5x less than OLSR on smart home topologies
- **Bootstrap cost**: TAVRN has lowest bootstrap cost in 14/17 scenarios
- **Energy**: TAVRN has lowest TX+RX energy in 12/17 scenarios
- **PDR**: TAVRN achieves 0.74-0.97 PDR (competitive but not best — AODV wins 12/17 due to aggressive retransmission)
- **Smart home highlight**: On home-medium, AODV generates 53,210 B/s overhead (43x more than TAVRN's 1,234 B/s) due to RREQ flooding through corridor chokepoints

## License

GPL-2.0 — see [LICENSE](LICENSE). This matches ns-3's license.

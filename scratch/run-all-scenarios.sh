#!/usr/bin/env bash
#
# TAVRN Full Scenario Sweep
# Runs 12 scenarios x N protocols x nRuns seeds, all in parallel (throttled).
#
# Usage:
#   bash scratch/run-all-scenarios.sh [nRuns] [simTime] [maxJobs] [--baseline=FILE] [--timeout=SECS]
#
# Defaults: nRuns=5, simTime=600, maxJobs=24, timeout=3600 (1hr)
# Output:   results/tavrn-sweep-<timestamp>.csv
#           results/tavrn-sweep-<timestamp>.txt
#
# With --baseline=FILE: only runs TAVRN, merges AODV/OLSR/DSDV from the
# baseline CSV. Saves ~75% of simulation time when iterating on TAVRN only.
#
set -euo pipefail

# Resolve ns-3 root directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NS3_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$NS3_ROOT"

NRUNS=5
SIMTIME=600
MAX_JOBS=24
BASELINE=""
JOB_TIMEOUT=3600  # Per-job wall-clock timeout in seconds (default: 1 hour)

# Parse all arguments: positional (nRuns simTime maxJobs) and --baseline=FILE
POS_ARGS=()
for arg in "$@"; do
  case "$arg" in
    --baseline=*) BASELINE="${arg#--baseline=}" ;;
    --timeout=*) JOB_TIMEOUT="${arg#--timeout=}" ;;
    *) POS_ARGS+=("$arg") ;;
  esac
done
[[ ${#POS_ARGS[@]} -ge 1 ]] && NRUNS="${POS_ARGS[0]}"
[[ ${#POS_ARGS[@]} -ge 2 ]] && SIMTIME="${POS_ARGS[1]}"
[[ ${#POS_ARGS[@]} -ge 3 ]] && MAX_JOBS="${POS_ARGS[2]}"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="results"
CSV_FILE="${RESULTS_DIR}/tavrn-sweep-${TIMESTAMP}.csv"
TABLE_FILE="${RESULTS_DIR}/tavrn-sweep-${TIMESTAMP}.txt"

if [[ -n "$BASELINE" ]]; then
  if [[ ! -f "$BASELINE" ]]; then
    echo "ERROR: baseline file not found: $BASELINE"
    exit 1
  fi
  PROTOCOLS=(TAVRN)
  echo "Baseline mode: only running TAVRN, merging AODV/OLSR/DSDV from $BASELINE"
else
  PROTOCOLS=(TAVRN AODV OLSR DSDV)
fi

mkdir -p "$RESULTS_DIR"

# ─────────────────────────────────────────────────────────────────────────────
# Scenario definitions: label|args
# ─────────────────────────────────────────────────────────────────────────────
SCENARIOS=(
  # A. Traffic load
  "sensor-dense|--scenario=grid --nNodes=25 --nFlows=10 --dataRate=20 --areaSize=500"
  "sensor-light|--scenario=grid --nNodes=25 --nFlows=2 --dataRate=1 --areaSize=500"

  # B. Network scale
  "small|--scenario=grid --nNodes=9 --nFlows=3 --areaSize=300"
  "medium|--scenario=grid --nNodes=25 --nFlows=5 --areaSize=500"
  "large|--scenario=grid --nNodes=49 --nFlows=10 --areaSize=700"

  # C. Mobility
  "mobile-slow|--scenario=mobile --nNodes=25 --nFlows=5 --areaSize=500 --maxSpeed=1.0 --pauseTime=5.0"
  "mobile-fast|--scenario=mobile --nNodes=25 --nFlows=5 --areaSize=500 --maxSpeed=5.0 --pauseTime=0.0"

  # D. Disruption / churn
  "node-failure|--scenario=grid --nNodes=25 --nFlows=5 --areaSize=500 --failNodes=5 --failTime=300"
  "node-rejoin|--scenario=grid --nNodes=25 --nFlows=5 --areaSize=500 --failNodes=5 --failTime=300 --rejoinTime=450"

  # E. Topology stress
  "sparse|--scenario=grid --nNodes=25 --nFlows=5 --areaSize=1000"
  "linear|--scenario=linear --nNodes=10 --nFlows=1 --areaSize=500"

  # F. Bursty traffic (IoT pattern)
  "bursty|--scenario=grid --nNodes=25 --nFlows=5 --areaSize=500 --onTime=5 --offTime=15"

  # Transient mobile: fast-moving nodes with rolling churn (cars entering/leaving)
  "mobile-churn|--scenario=mobile --nNodes=25 --nFlows=5 --areaSize=500 --maxSpeed=5.0 --pauseTime=0.0 --churnNodes=5 --churnInterval=60"

  # G. Smart home: room-corridor topology with mixed hub+peer traffic
  "home-small|--scenario=home --nNodes=15 --nFlows=5 --dataRate=1 --hubTraffic=true"
  "home-medium|--scenario=home --nNodes=30 --nFlows=8 --dataRate=1 --hubTraffic=true"
  "home-large|--scenario=home --nNodes=50 --nFlows=12 --dataRate=1 --hubTraffic=true"
  "home-hell|--scenario=home --nNodes=50 --nFlows=12 --dataRate=1 --hubTraffic=true --churnNodes=1 --churnInterval=60"
)

TOTAL_SCENARIOS=${#SCENARIOS[@]}
TOTAL_JOBS=$((TOTAL_SCENARIOS * ${#PROTOCOLS[@]}))

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  TAVRN Full Scenario Sweep                                  ║"
echo "║  ${TOTAL_JOBS} sim jobs (${TOTAL_SCENARIOS} scenarios × ${#PROTOCOLS[@]} protocol(s) × ${NRUNS} seeds)  ║"
if [[ -n "$BASELINE" ]]; then
echo "║  Baseline: $(basename "$BASELINE")                          ║"
fi
echo "║  simTime=${SIMTIME}s  maxParallel=${MAX_JOBS}  timeout=${JOB_TIMEOUT}s            ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# Temp dir for per-job output
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# ─────────────────────────────────────────────────────────────────────────────
# Launch all jobs with FIFO-based semaphore (bulletproof throttle)
# ─────────────────────────────────────────────────────────────────────────────
COMPLETED=0
FAILED=0
START_TIME=$SECONDS

# Create FIFO semaphore: pre-fill with MAX_JOBS tokens
FIFO="${TMPDIR}/jobslots"
mkfifo "$FIFO"
exec 3<>"$FIFO"  # open read+write on fd 3
for (( i=0; i<MAX_JOBS; i++ )); do
  echo "x" >&3   # add a token
done

JOB_NUM=0
for SCENARIO_DEF in "${SCENARIOS[@]}"; do
  LABEL="${SCENARIO_DEF%%|*}"
  ARGS="${SCENARIO_DEF#*|}"

  for PROTO in "${PROTOCOLS[@]}"; do
    OUTFILE="${TMPDIR}/${LABEL}_${PROTO}.csv"
    LOGFILE="${TMPDIR}/${LABEL}_${PROTO}.log"

    # Acquire a token (blocks if all slots are taken)
    read -u 3

    JOB_NUM=$((JOB_NUM + 1))
    echo "  [${JOB_NUM}/${TOTAL_JOBS}] START ${LABEL} / ${PROTO}"

    # Launch job — releases token when done
    # Uses timeout to kill jobs that exceed JOB_TIMEOUT (e.g. AODV RREQ storms)
    (
      if OUTPUT=$(timeout --signal=KILL "${JOB_TIMEOUT}" ./ns3 run "tavrn-comparison --protocol=${PROTO} ${ARGS} --simTime=${SIMTIME} --nRuns=${NRUNS}" 2>"$LOGFILE"); then
        DATA_LINE=$(echo "$OUTPUT" | tail -1)
        echo "${LABEL},${DATA_LINE}" > "$OUTFILE"
        echo "  [DONE] ${LABEL} / ${PROTO}"
      else
        EXIT_CODE=$?
        if [[ $EXIT_CODE -eq 137 ]]; then
          # Killed by timeout — generate DNF row
          # Extract scenario params from ARGS for the CSV row
          SCEN=$(echo "${ARGS}" | grep -oP '(?<=--scenario=)\S+' || echo "unknown")
          NNODES=$(echo "${ARGS}" | grep -oP '(?<=--nNodes=)\S+' || echo "0")
          NFLOWS=$(echo "${ARGS}" | grep -oP '(?<=--nFlows=)\S+' || echo "0")
          # DNF row: -1 for all metrics (easily filterable)
          echo "${LABEL},${PROTO},${SCEN},${NNODES},${NFLOWS},${SIMTIME},${NRUNS},-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0" > "$OUTFILE"
          echo "  [DNF]  ${LABEL} / ${PROTO} (killed after ${JOB_TIMEOUT}s — likely packet storm)"
        else
          echo "  [FAIL] ${LABEL} / ${PROTO} (exit ${EXIT_CODE}, see ${LOGFILE})"
        fi
      fi
      echo "x" >&3  # release token
    ) &
  done
done

echo ""
echo "All ${TOTAL_JOBS} jobs submitted. Waiting for stragglers..."
echo ""
wait
exec 3>&-  # close fd

ELAPSED=$((SECONDS - START_TIME))
echo ""
echo "All jobs finished in ${ELAPSED}s ($((ELAPSED / 60))m $((ELAPSED % 60))s)"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Collect results in scenario order
# ─────────────────────────────────────────────────────────────────────────────
HEADER="scenarioLabel,protocol,scenario,nNodes,nFlows,simTime,nRuns,pdr_mean,pdr_ci95,latency_mean,latency_ci95,steadyBps_mean,steadyBps_ci95,bootstrapBytes_mean,bootstrapBytes_ci95,totalOverhead_mean,totalOverhead_ci95,energy_mean,energy_ci95,txEnergy_mean,txEnergy_ci95,rxEnergy_mean,rxEnergy_ci95,gttAccuracy_mean,gttAccuracy_ci95,convergence_mean,convergence_ci95,routeDiscovery_mean,routeDiscovery_ci95"
echo "$HEADER" > "$CSV_FILE"

# Build a lookup of baseline rows (if provided) keyed on "scenarioLabel,protocol"
declare -A BASELINE_ROWS
if [[ -n "$BASELINE" ]]; then
  while IFS= read -r line; do
    BL_LABEL=$(echo "$line" | cut -d',' -f1)
    BL_PROTO=$(echo "$line" | cut -d',' -f2)
    BASELINE_ROWS["${BL_LABEL}_${BL_PROTO}"]="$line"
  done < <(tail -n +2 "$BASELINE")  # skip header
fi

ALL_PROTOS=(TAVRN AODV OLSR DSDV)

for SCENARIO_DEF in "${SCENARIOS[@]}"; do
  LABEL="${SCENARIO_DEF%%|*}"
  for PROTO in "${ALL_PROTOS[@]}"; do
    OUTFILE="${TMPDIR}/${LABEL}_${PROTO}.csv"
    if [[ -f "$OUTFILE" ]]; then
      # Fresh simulation result
      cat "$OUTFILE" >> "$CSV_FILE"
      COMPLETED=$((COMPLETED + 1))
    elif [[ -n "$BASELINE" && -n "${BASELINE_ROWS["${LABEL}_${PROTO}"]:-}" ]]; then
      # Use baseline result for non-TAVRN protocols
      echo "${BASELINE_ROWS["${LABEL}_${PROTO}"]}" >> "$CSV_FILE"
      COMPLETED=$((COMPLETED + 1))
    else
      echo "WARNING: missing result for ${LABEL}/${PROTO}"
      # Generate a missing row so the CSV is always complete
      SCEN_ARGS="${SCENARIO_DEF#*|}"
      M_SCEN=$(echo "${SCEN_ARGS}" | grep -oP '(?<=--scenario=)\S+' || echo "unknown")
      M_NNODES=$(echo "${SCEN_ARGS}" | grep -oP '(?<=--nNodes=)\S+' || echo "0")
      M_NFLOWS=$(echo "${SCEN_ARGS}" | grep -oP '(?<=--nFlows=)\S+' || echo "0")
      echo "${LABEL},${PROTO},${M_SCEN},${M_NNODES},${M_NFLOWS},${SIMTIME},${NRUNS},-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0" >> "$CSV_FILE"
      FAILED=$((FAILED + 1))
    fi
  done
done

TOTAL_EXPECTED=$((TOTAL_SCENARIOS * ${#ALL_PROTOS[@]}))
echo ""
echo "Results: ${COMPLETED}/${TOTAL_EXPECTED} succeeded, ${FAILED} failed"
if [[ -n "$BASELINE" ]]; then
  echo "  (TAVRN: fresh runs, AODV/OLSR/DSDV: from baseline)"
fi
echo "CSV: ${CSV_FILE}"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Generate formatted summary table
# ─────────────────────────────────────────────────────────────────────────────
echo "Generating summary table..."

python3 - "$CSV_FILE" "$TABLE_FILE" << 'PYEOF'
import csv
import sys
from collections import defaultdict

csv_path = sys.argv[1]
table_path = sys.argv[2]

rows = []
with open(csv_path) as f:
    reader = csv.DictReader(f)
    for row in reader:
        rows.append(row)

if not rows:
    print("No results found!")
    sys.exit(1)

# Preserve scenario order
scenarios = []
seen = set()
for r in rows:
    label = r["scenarioLabel"]
    if label not in seen:
        scenarios.append(label)
        seen.add(label)

protocols = ["TAVRN", "AODV", "OLSR", "DSDV"]

metrics = [
    ("PDR",            "pdr_mean",            "pdr_ci95",            "{:.2f}"),
    ("Latency(ms)",    "latency_mean",        "latency_ci95",        "{:.1f}"),
    ("Steady(B/s)",    "steadyBps_mean",      "steadyBps_ci95",      "{:.0f}"),
    ("Bootstrap(B)",   "bootstrapBytes_mean",  "bootstrapBytes_ci95", "{:.0f}"),
    ("TX+RX(J)",       None,                   None,                  "{:.1f}"),
    ("GTT Acc",        "gttAccuracy_mean",     "gttAccuracy_ci95",    "{:.2f}"),
]

lookup = {}
for r in rows:
    lookup[(r["scenarioLabel"], r["protocol"])] = r

lines = []

def add(s):
    lines.append(s)
    print(s)

add("=" * 100)
add("TAVRN FULL SCENARIO COMPARISON")
add("=" * 100)
add("")

for scenario in scenarios:
    add(f"--- {scenario} ---")
    first = lookup.get((scenario, "TAVRN")) or lookup.get((scenario, "AODV"))
    if first:
        add(f"    nodes={first.get('nNodes','?')} flows={first.get('nFlows','?')} simTime={first.get('simTime','?')}s nRuns={first.get('nRuns','?')}")
    add("")

    hdr = f"  {'Metric':<16}"
    for p in protocols:
        hdr += f"  {p:>16}"
    add(hdr)
    add("  " + "-" * (16 + 4 * 18))

    for metric_name, mean_key, ci_key, fmt in metrics:
        line = f"  {metric_name:<16}"
        for p in protocols:
            r = lookup.get((scenario, p))
            if not r:
                line += f"  {'N/A':>16}"
                continue

            # Check if this is a DNF row (all metrics are -1)
            is_dnf = False
            try:
                if float(r.get("pdr_mean", 0)) == -1:
                    is_dnf = True
            except (ValueError, TypeError):
                pass

            if is_dnf:
                cell = "DNF"
            elif metric_name == "TX+RX(J)":
                try:
                    tx = float(r.get("txEnergy_mean", 0))
                    rx = float(r.get("rxEnergy_mean", 0))
                    tx_ci = float(r.get("txEnergy_ci95", 0))
                    rx_ci = float(r.get("rxEnergy_ci95", 0))
                    val = tx + rx
                    ci = (tx_ci**2 + rx_ci**2)**0.5
                    cell = fmt.format(val)
                    if ci > 0:
                        cell += f"+/-{ci:.1f}"
                except (ValueError, TypeError):
                    cell = "N/A"
            else:
                try:
                    val = float(r[mean_key])
                    if metric_name == "GTT Acc" and val < 0:
                        cell = "--"
                    else:
                        cell = fmt.format(val)
                        ci = float(r.get(ci_key, 0))
                        if ci > 0:
                            cell += f"+/-{fmt.format(ci)}"
                except (ValueError, TypeError, KeyError):
                    cell = "N/A"

            line += f"  {cell:>16}"
        add(line)
    add("")

# Count DNF rows
dnf_count = sum(1 for r in rows if float(r.get("pdr_mean", 0)) == -1)
if dnf_count > 0:
    add(f"NOTE: {dnf_count} job(s) marked DNF (exceeded wall-clock timeout)")
    add("  DNF jobs are excluded from wins calculation.")
    add("")

add("=" * 100)
add("WINS SUMMARY (best value per scenario)")
add("=" * 100)
wins = defaultdict(lambda: defaultdict(int))
comparable_metrics = [
    ("PDR",          "pdr_mean",       "max"),
    ("Latency",      "latency_mean",   "min"),
    ("Steady B/s",   "steadyBps_mean", "min"),
    ("Bootstrap B",  "bootstrapBytes_mean", "min"),
    ("TX+RX Energy", None,             "min"),
]

for scenario in scenarios:
    for mname, mkey, direction in comparable_metrics:
        best_val = None
        best_proto = None
        for p in protocols:
            r = lookup.get((scenario, p))
            if not r:
                continue
            # Skip DNF rows from wins calculation
            try:
                if float(r.get("pdr_mean", 0)) == -1:
                    continue
            except (ValueError, TypeError):
                pass
            try:
                if mname == "TX+RX Energy":
                    val = float(r.get("txEnergy_mean", 0)) + float(r.get("rxEnergy_mean", 0))
                else:
                    val = float(r[mkey])
                if val <= 0 and mname == "PDR":
                    continue
                if best_val is None:
                    best_val = val
                    best_proto = p
                elif direction == "max" and val > best_val:
                    best_val = val
                    best_proto = p
                elif direction == "min" and val < best_val:
                    best_val = val
                    best_proto = p
            except (ValueError, TypeError, KeyError):
                continue
        if best_proto:
            wins[best_proto][mname] += 1

add("")
hdr = f"  {'Metric':<16}"
for p in protocols:
    hdr += f"  {p:>8}"
add(hdr)
add("  " + "-" * (16 + 4 * 10))
for mname, _, _ in comparable_metrics:
    line = f"  {mname:<16}"
    for p in protocols:
        count = wins[p][mname]
        cell = f"{count}/{len(scenarios)}"
        line += f"  {cell:>8}"
    add(line)
add("")

with open(table_path, "w") as f:
    f.write("\n".join(lines) + "\n")

print(f"\nTable written to: {table_path}")
PYEOF

echo ""
echo "Done! Files:"
echo "  CSV:   ${CSV_FILE}"
echo "  Table: ${TABLE_FILE}"

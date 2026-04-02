#!/usr/bin/env bash
set -euo pipefail

# ---- Defaults (override via env vars) ----
OUT_PREFIX="${OUT_PREFIX:-data/run1}"
GPU_INDEX="${GPU_INDEX:-0}"
INTERVAL="${INTERVAL:-1}"

IDLE_S="${IDLE_S:-180}"
LOAD_S="${LOAD_S:-300}"
COOLDOWN_S="${COOLDOWN_S:-180}"
STEADY_TRIM_S="${STEADY_TRIM_S:-30}"

GEMM_N="${GEMM_N:-8192}"
GEMM_DTYPE="${GEMM_DTYPE:-bf16}"

NVML_CSV="${OUT_PREFIX}_nvml.csv"
GEMM_CSV="${OUT_PREFIX}_gemm.csv"
PHASES_JSON="${OUT_PREFIX}_phases.json"

mkdir -p "$(dirname "$OUT_PREFIX")"

echo "== GPU Health Index: run_experiment =="
echo "Output prefix:       $OUT_PREFIX"
echo "GPU index:           $GPU_INDEX"
echo "Sampling interval:   ${INTERVAL}s"
echo "Idle/Load/Cooldown:  ${IDLE_S}s / ${LOAD_S}s / ${COOLDOWN_S}s"
echo "Steady trim:         ${STEADY_TRIM_S}s"
echo "GEMM:                N=${GEMM_N} dtype=${GEMM_DTYPE}"
echo

# Write phases config (used by analyze.py)
cat > "$PHASES_JSON" <<JSON
{
  "idle_s": ${IDLE_S},
  "load_s": ${LOAD_S},
  "cooldown_s": ${COOLDOWN_S},
  "steady_trim_s": ${STEADY_TRIM_S}
}
JSON

echo "Wrote phases: $PHASES_JSON"

# Start collector in background
echo "Starting collector -> $NVML_CSV"
./collector.py --out "$NVML_CSV" --interval "$INTERVAL" --gpu "$GPU_INDEX" &
COLLECTOR_PID=$!

cleanup() {
  echo
  echo "Stopping collector (pid=$COLLECTOR_PID)..."
  kill -TERM "$COLLECTOR_PID" 2>/dev/null || true
  wait "$COLLECTOR_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "Phase 1/3: idle (${IDLE_S}s)"
sleep "$IDLE_S"

echo "Phase 2/3: load (${LOAD_S}s) -> $GEMM_CSV"
./stress_torch_gemm.py --seconds "$LOAD_S" --n "$GEMM_N" --dtype "$GEMM_DTYPE" --out "$GEMM_CSV"

echo "Phase 3/3: cooldown (${COOLDOWN_S}s)"
sleep "$COOLDOWN_S"

# Collector stopped by trap/cleanup after this point when script exits
echo
echo "Analyzing..."
./analyze.py --nvml "$NVML_CSV" --gemm "$GEMM_CSV" --phases "$PHASES_JSON" --out_prefix "$OUT_PREFIX"

echo
echo "Done."
echo "Artifacts:"
echo " - $NVML_CSV"
echo " - $GEMM_CSV"
echo " - ${OUT_PREFIX}_merged.csv"
echo " - ${OUT_PREFIX}_report.md"
echo " - ${OUT_PREFIX}_power.png / _temp.png / _sm_clock.png / _iters_per_sec.png / _perf_per_watt.png"

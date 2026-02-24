#!/usr/bin/env bash
set -euo pipefail

PROM_URL="${PROM_URL:-http://127.0.0.1:9090}"
GPU_INDEX="${GPU_INDEX:-0}"
INTERVAL_S="${INTERVAL_S:-1}"

OUT_PREFIX="${OUT_PREFIX:-data/promrun1}"

IDLE_S="${IDLE_S:-180}"
LOAD_S="${LOAD_S:-300}"
COOLDOWN_S="${COOLDOWN_S:-180}"
STEADY_TRIM_S="${STEADY_TRIM_S:-30}"

GEMM_N="${GEMM_N:-8192}"
GEMM_DTYPE="${GEMM_DTYPE:-bf16}"

PHASES_JSON="${OUT_PREFIX}_phases.json"
GEMM_CSV="${OUT_PREFIX}_gemm.csv"
PROM_NVML_CSV="${OUT_PREFIX}_nvml_prom.csv"

BASELINE="${BASELINE:-data/baseline.json}"

echo "== GPU Health Index: run_experiment_prom =="
echo "Output prefix:       ${OUT_PREFIX}"
echo "GPU index:           ${GPU_INDEX}"
echo "Sampling interval:   ${INTERVAL_S}s"
echo "Idle/Load/Cooldown:  ${IDLE_S}s / ${LOAD_S}s / ${COOLDOWN_S}s"
echo "Steady trim:         ${STEADY_TRIM_S}s"
echo "GEMM:                N=${GEMM_N} dtype=${GEMM_DTYPE}"
echo "Prometheus:          ${PROM_URL}"
echo

mkdir -p "$(dirname "${OUT_PREFIX}")"

cat > "${PHASES_JSON}" <<JSON
{"idle_s":${IDLE_S},"load_s":${LOAD_S},"cooldown_s":${COOLDOWN_S},"steady_trim_s":${STEADY_TRIM_S}}
JSON
echo "Wrote phases: ${PHASES_JSON}"

# Resolve UUID for this GPU index (stable identifier)
GPU_UUID="$(nvidia-smi --query-gpu=uuid --format=csv,noheader | sed -n "$((GPU_INDEX+1))p" | tr -d '[:space:]')"
if [[ -z "${GPU_UUID}" ]]; then
  echo "ERROR: could not resolve GPU UUID for GPU_INDEX=${GPU_INDEX}" >&2
  exit 2
fi
echo "GPU UUID:            ${GPU_UUID}"

# Record experiment window boundaries (epoch seconds, UTC)
t0=$(date -u +%s)
echo "t0_epoch=${t0} (UTC)"

echo "Phase 1/3: idle (${IDLE_S}s)"
sleep "${IDLE_S}"

echo "Phase 2/3: load (${LOAD_S}s) -> ${GEMM_CSV}"
python3 -u stress_torch_gemm.py --seconds "${LOAD_S}" --n "${GEMM_N}" --dtype "${GEMM_DTYPE}" --out "${GEMM_CSV}"
echo "Wrote ${GEMM_CSV}"

echo "Phase 3/3: cooldown (${COOLDOWN_S}s)"
sleep "${COOLDOWN_S}"

t1=$(date -u +%s)
echo "t1_epoch=${t1} (UTC)"

# Export Prom telemetry for the window with small guard bands
START=$((t0 - 5))
END=$((t1 + 5))

# Expected samples (rough): window_seconds / step
WINDOW_S=$((END - START))
EXPECTED=$(( WINDOW_S / INTERVAL_S ))
MIN_ROWS=$(( EXPECTED * 80 / 100 ))   # require >=80% of expected

echo
echo "Exporting Prom telemetry -> ${PROM_NVML_CSV}"
./prom_export.py \
  --prom "${PROM_URL}" \
  --uuid "${GPU_UUID}" \
  --start "${START}" \
  --end "${END}" \
  --step "${INTERVAL_S}" \
  --min_rows "${MIN_ROWS}" \
  --max_median_dt 2.5 \
  --out "${PROM_NVML_CSV}"

echo
echo "Analyzing..."
ANALYZE_ARGS=( --nvml "${PROM_NVML_CSV}" --gemm "${GEMM_CSV}" --phases "${PHASES_JSON}" --out_prefix "${OUT_PREFIX}" )
if [[ -f "${BASELINE}" ]]; then
  ANALYZE_ARGS+=( --baseline "${BASELINE}" )
fi

./analyze.py "${ANALYZE_ARGS[@]}"

echo
echo "Done."
echo "Artifacts:"
echo " - ${PROM_NVML_CSV}"
echo " - ${GEMM_CSV}"
echo " - ${OUT_PREFIX}_merged.csv"
echo " - ${OUT_PREFIX}_report.md"
echo " - ${OUT_PREFIX}_power.png / _temp.png / _sm_clock.png / _iters_per_sec.png / _perf_per_watt.png"

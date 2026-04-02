#!/usr/bin/env bash
set -euo pipefail

PHASES="${PHASES:-data/run1_phases.json}"
BASELINE_OUT="${BASELINE_OUT:-data/baseline.json}"
REPLAY_DIR="${REPLAY_DIR:-data/replay}"

echo "== GPU Health Index: replay demo =="

echo "[1/3] Build baseline -> $BASELINE_OUT"
./make_baseline.py --merged data/run1_merged.csv data/run2_merged.csv data/run3_merged.csv --phases "$PHASES" --out "$BASELINE_OUT"

echo "[2/3] Generate replay datasets -> $REPLAY_DIR"
python3 -u make_replay.py --in data/run1_merged.csv --out_dir "$REPLAY_DIR"

echo "[3/3] Analyze replay datasets"
./analyze_merged.py --merged "$REPLAY_DIR/degrading_efficiency_10pct.csv"     --phases "$PHASES" --baseline "$BASELINE_OUT" --out_prefix "$REPLAY_DIR/out_eff_10pct"
./analyze_merged.py --merged "$REPLAY_DIR/degrading_clocks_8pct.csv"         --phases "$PHASES" --baseline "$BASELINE_OUT" --out_prefix "$REPLAY_DIR/out_clk_8pct"
./analyze_merged.py --merged "$REPLAY_DIR/degrading_combined_strong.csv"     --phases "$PHASES" --baseline "$BASELINE_OUT" --out_prefix "$REPLAY_DIR/out_combo_strong"
./analyze_merged.py --merged "$REPLAY_DIR/degrading_combined_severe.csv"     --phases "$PHASES" --baseline "$BASELINE_OUT" --out_prefix "$REPLAY_DIR/out_combo_severe"

echo
echo "Done. See $REPLAY_DIR/out_*_report.md"

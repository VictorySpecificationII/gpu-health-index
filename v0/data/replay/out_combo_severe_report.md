# GPU Health Index Report (v0) — Replay

- Phase selection mode: **phases.json**
- Baseline: **data/baseline.json**
- Classification: **Degrading**
- Health Score: **57.0 / 100**

## Load Window Summary (steady-state)

- Samples: 270
- Power mean: 692.17 W
- Power limit: 700.00 W
- Temp p95: 82.84 C
- SM clock std: 58.49 MHz
- Perf/W mean: 0.733182
- Perf/W CoV: 0.093507
- Baseline perf/W: 0.874878
- Perf/W drop vs baseline: 16.20%

## Notes

- Thermal: p95 temp 82.8C (penalty 10)
- Power headroom: 100.0% samples ≥ 98.0% of limit (692.2W mean, limit 700.0W) (penalty 3.0)
- Degradation: perf/W mean 0.733182 vs baseline 0.874878 (16.2% drop) (penalty 30)

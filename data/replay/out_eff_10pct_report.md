# GPU Health Index Report (v0) — Replay

- Phase selection mode: **phases.json**
- Baseline: **data/baseline.json**
- Classification: **Healthy**
- Health Score: **92.0 / 100**

## Load Window Summary (steady-state)

- Samples: 270
- Power mean: 692.17 W
- Power limit: 700.00 W
- Temp p95: 64.00 C
- SM clock std: 12.02 MHz
- Perf/W mean: 0.827613
- Perf/W CoV: 0.028086
- Baseline perf/W: 0.874878
- Perf/W drop vs baseline: 5.40%

## Notes

- Power headroom: 100.0% samples ≥ 98.0% of limit (692.2W mean, limit 700.0W) (penalty 3.0)
- Degradation: perf/W mean 0.827613 vs baseline 0.874878 (5.4% drop) (penalty 5)

# GPU Health Index Report (v0) — Replay

- Phase selection mode: **phases.json**
- Baseline: **data/baseline.json**
- Classification: **Monitor**
- Health Score: **82.0 / 100**

## Load Window Summary (steady-state)

- Samples: 270
- Power mean: 692.17 W
- Power limit: 700.00 W
- Temp p95: 78.13 C
- SM clock std: 40.32 MHz
- Perf/W mean: 0.780397
- Perf/W CoV: 0.058805
- Baseline perf/W: 0.874878
- Perf/W drop vs baseline: 10.80%

## Notes

- Power headroom: 100.0% samples ≥ 98.0% of limit (692.2W mean, limit 700.0W) (penalty 3.0)
- Degradation: perf/W mean 0.780397 vs baseline 0.874878 (10.8% drop) (penalty 15)

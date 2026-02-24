# GPU Health Index Report (v0)

- Phase selection mode: **phases.json**
- Effective load filter: **power>=0.90*limit**
- Baseline: **data/baseline.json**
- Classification: **Healthy**
- Health Score: **97.0 / 100**

## Load Window Summary (steady-state)

- Samples (steady-state): 270
- Samples (effective load): 270
- Power mean: 691.66 W
- Power limit: 700.00 W
- Power ratio (mean): 98.81%
- Power saturation: 100.00% samples ≥ 98.0% of limit
- Temp p95: 74.00 C
- SM clock std: 25.81 MHz
- Perf/W mean: 0.883582
- Perf/W CoV: 0.005904
- Baseline perf/W: 0.874878
- Perf/W drop vs baseline: -0.99%

## Notes

- Power headroom: 100.0% samples ≥ 98.0% of limit (691.7W mean, limit 700.0W) (penalty 3.0)
- Baseline: perf/W mean 0.883582 vs baseline 0.874878 (-1.0% drop)

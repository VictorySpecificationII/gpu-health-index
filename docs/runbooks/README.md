# GPU Health Exporter — Runbooks

One runbook per alert. All alerts are defined in
`deploy/monitoring/prometheus/alerts.yml`.

**Quick reference commands**

```sh
# Exporter status and recent logs
systemctl status gpu-health.service
journalctl -u gpu-health.service -n 100 --no-pager

# Live metrics from the exporter
curl -s localhost:9108/metrics          # plain HTTP
curl -sk https://localhost:9108/metrics # TLS

# GPU hardware state
nvidia-smi
nvidia-smi -q -d ECC
nvidia-smi -q -d MEMORY
```

---

## Alert index

| Alert | Severity | Condition | Runbook |
|---|---|---|---|
| GpuExporterDown | critical | Exporter unreachable for 1m | [gpu-exporter-down.md](gpu-exporter-down.md) |
| GpuUnavailable | critical | NVML unavailable for 1m | [gpu-unavailable.md](gpu-unavailable.md) |
| GpuDcgmUnavailable | critical | DCGM unavailable for 2m | [gpu-dcgm-unavailable.md](gpu-dcgm-unavailable.md) |
| GpuDecommissionCandidate | critical | health_class == 4 for 5m | [gpu-decommission-candidate.md](gpu-decommission-candidate.md) |
| GpuDegrading | warning | health_class == 3 for 5m | [gpu-degrading.md](gpu-degrading.md) |
| GpuTelemetryIncomplete | warning | telemetry_ok == 0 for 5m | [gpu-telemetry-incomplete.md](gpu-telemetry-incomplete.md) |
| GpuEccDoubleBitError | critical | DBE in scoring window for 1m | [gpu-ecc-double-bit-error.md](gpu-ecc-double-bit-error.md) |
| GpuHighEccSbeRate | warning | SBE rate > 100/hr for 5m | [gpu-high-ecc-sbe-rate.md](gpu-high-ecc-sbe-rate.md) |
| GpuRowRemapFailure | critical | row remap failures > 0 for 1m | [gpu-row-remap-failure.md](gpu-row-remap-failure.md) |
| GpuPcieLinkDegraded | warning | current gen/width < max for 2m | [gpu-pcie-link-degraded.md](gpu-pcie-link-degraded.md) |
| GpuProbeResultStale | warning | probe result older than TTL for 25h | [gpu-probe-result-stale.md](gpu-probe-result-stale.md) |

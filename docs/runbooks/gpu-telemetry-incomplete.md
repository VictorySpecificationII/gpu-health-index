# GpuTelemetryIncomplete

**Severity:** warning  
**Fires when:** `gpu_telemetry_ok == 0` for 5 minutes — fewer samples than
expected in the scoring window. Score is suppressed (class 0 / N/A).

## Impact

Health score is not being computed. This GPU will not trigger score-based
alerts (`GpuDegrading`, `GpuDecommissionCandidate`) until telemetry recovers.

## Diagnosis

1. **When did the last poll succeed?**
   ```sh
   curl -s localhost:9108/metrics | grep last_poll_timestamp
   ```
   Compare against `date +%s`. Difference > `poll_interval_s × 3` → poll stall.

2. **Is the GPU still present?**
   ```sh
   curl -s localhost:9108/metrics | grep gpu_present
   nvidia-smi
   ```

3. **NVML error rate:**
   ```sh
   curl -s localhost:9108/metrics | grep collector_errors_total
   ```

4. **Exporter logs for poll thread activity:**
   ```sh
   journalctl -u gpu-health.service -n 200 --no-pager | grep "collector\["
   ```

## Remediation

| Root cause | Action |
|---|---|
| Poll thread stalled | `sudo systemctl restart gpu-health.service` |
| GPU disappeared (`gpu_present == 0`) | Follow [gpu-unavailable.md](gpu-unavailable.md) |
| Exporter just started | Wait one full `window_s` (default 300s) for window to fill |

## Escalation

Poll thread stall recurs after restart → possible kernel or driver issue;
escalate to GPU infrastructure owner.

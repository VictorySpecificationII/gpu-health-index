# GpuExporterDown

**Severity:** critical  
**Fires when:** `up{job="gpu-health"} == 0` for 1 minute — Prometheus cannot
scrape the exporter endpoint.

## Impact

All GPU health metrics are stale. `GpuUnavailable`, `GpuDcgmUnavailable`, and
all score-based alerts will not fire while the exporter is down. Blind spot on
all GPUs on this host.

## Diagnosis

1. **Is the service running?**
   ```sh
   systemctl status gpu-health.service
   ```
   - `active (running)` → port or network issue; go to step 3.
   - `failed` or `inactive` → go to step 2.

2. **Why did it exit?**
   ```sh
   journalctl -u gpu-health.service -n 200 --no-pager
   ```
   Look for: `bind failed` (port conflict), `NVML init failed` (driver issue),
   `seccomp` or `capset` errors (permission regression), OOM kill.

3. **Is the port open?**
   ```sh
   curl -sv localhost:9108/ready
   ss -tlnp | grep 9108
   ```

4. **Is Prometheus reaching the host?**
   Check `file_sd.json` is present and contains the correct address:
   ```sh
   cat /var/run/gpu-health/file_sd.json
   ```

## Remediation

| Root cause | Action |
|---|---|
| Service crashed (no driver change) | `sudo systemctl restart gpu-health.service` |
| Port conflict | Identify conflicting process: `ss -tlnp \| grep 9108`; resolve before restarting |
| Driver reload / nvidia-smi not responding | Reboot the host; re-enable service after reboot |
| file_sd.json missing | Restart service — it writes the file at startup |

## Escalation

Restart fails or recurs within 1 hour → escalate to GPU infrastructure owner.

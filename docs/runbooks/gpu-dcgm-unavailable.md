# GpuDcgmUnavailable

**Severity:** critical  
**Fires when:** `gpu_dcgm_available == 0` for 2 minutes — the exporter's DCGM
connection has failed or DCGM is not responding.

## Impact

Board power, total energy, memory bandwidth utilisation, and violation counters
are unavailable (`NaN` in metrics). The health score's DCGM-sourced components
are dropped. DCGM is operationally required on this fleet — this is not an
acceptable degraded mode.

## Diagnosis

1. **Is the DCGM daemon running?**
   ```sh
   systemctl status nvidia-dcgm.service
   ```

2. **DCGM daemon logs:**
   ```sh
   journalctl -u nvidia-dcgm.service -n 100 --no-pager
   ```

3. **Can DCGM see the GPUs?**
   ```sh
   dcgmi discovery -l
   ```

4. **Check exporter logs for DCGM error detail:**
   ```sh
   journalctl -u gpu-health.service -n 100 --no-pager | grep -i dcgm
   ```

## Remediation

| Root cause | Action |
|---|---|
| DCGM daemon stopped | `sudo systemctl restart nvidia-dcgm.service`; verify `dcgmi discovery -l` |
| DCGM daemon not starting | Check driver compatibility: `dcgmi --version` vs driver version |
| Exporter lost connection after DCGM restart | `sudo systemctl restart gpu-health.service` |
| Fabric Manager not running (NVSwitch systems) | `sudo systemctl start nvidia-fabricmanager.service` |

## Escalation

DCGM fails to start after driver check → escalate to GPU infrastructure owner.

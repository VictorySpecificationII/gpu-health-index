# GpuUnavailable

**Severity:** critical  
**Fires when:** `gpu_available == 0` for 1 minute — the exporter has exceeded
its NVML error threshold for this GPU.

## Impact

NVML is no longer responding for this device. Health score is frozen; ECC,
temperature, and utilisation metrics are stale. Workloads on this GPU may be
affected.

## Diagnosis

1. **Is the GPU visible to the driver?**
   ```sh
   nvidia-smi
   ```
   - GPU missing from output → likely fell off the bus (XID 79). Go to step 4.
   - GPU present → NVML error streak; go to step 2.

2. **Check exporter error count:**
   ```sh
   curl -s localhost:9108/metrics | grep collector_errors_total
   ```
   A rapidly climbing counter confirms ongoing NVML failures.

3. **Check for XID errors in the kernel log:**
   ```sh
   journalctl -k --since "1 hour ago" | grep -i "xid\|nvidia\|nvrm"
   dmesg | grep -i "xid\|nvidia" | tail -20
   ```
   - **XID 48** — DBE / uncorrectable ECC error. See [gpu-ecc-double-bit-error.md](gpu-ecc-double-bit-error.md).
   - **XID 79** — GPU fell off the bus. Hardware fault; reboot required.
   - **XID 31** — GPU MMU fault. Drain workloads; investigate application.

4. **GPU fell off the bus (XID 79):**
   Drain all workloads on the host. Schedule reboot. After reboot check:
   ```sh
   nvidia-smi -q -d ECC
   nvidia-smi -q | grep "Retired Pages"
   ```

## Remediation

| Root cause | Action |
|---|---|
| Transient NVML error streak | `sudo systemctl restart gpu-health.service`; monitor `gpu_available` |
| XID 79 (GPU off bus) | Drain workloads → reboot → post-reboot ECC check |
| XID 48 (DBE) | Follow [gpu-ecc-double-bit-error.md](gpu-ecc-double-bit-error.md) |
| Driver wedge (nvidia-smi hangs) | Reboot; notify GPU infrastructure owner |

## Escalation

GPU does not recover after reboot → hardware replacement; escalate to
data-centre ops.

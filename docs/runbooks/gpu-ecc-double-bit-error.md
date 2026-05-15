# GpuEccDoubleBitError

**Severity:** critical  
**Fires when:** `gpu_ecc_dbe_in_window == 1` for 1 minute — at least one ECC
double-bit error delta was observed in the scoring window.

## Impact

Uncorrectable memory corruption has occurred. Any workload currently running
on this GPU may have produced incorrect results. Immediate action required.

## Diagnosis

1. **Confirm DBE count:**
   ```sh
   nvidia-smi -q -d ECC | grep -A 30 "ECC Errors"
   curl -s localhost:9108/metrics | grep -E "ecc_dbe|ecc_sbe"
   ```

2. **Check for XID 48 in kernel log:**
   ```sh
   dmesg | grep -i "xid" | tail -20
   journalctl -k --since "2 hours ago" | grep "xid"
   ```

3. **Check retired pages — DBEs cause page retirement:**
   ```sh
   nvidia-smi -q | grep -A 5 "Retired Pages"
   ```

4. **Is a reboot needed to retire the page?**
   ```sh
   curl -s localhost:9108/metrics | grep pending_row_remap
   nvidia-smi -q | grep "Pending Retirement"
   ```

## Remediation

1. **Drain all workloads from this GPU immediately.**
2. Notify users of possible result corruption.
3. If pending page retirement:
   ```sh
   sudo reboot
   ```
   After reboot, re-check retired page count.
4. If DBE count continues to climb after reboot → **decommission**. ECC DBEs
   that persist across reboots indicate permanent memory cell damage.
5. If this is a single isolated DBE with no recurrence and no pending
   retirement → monitor closely; run fresh probe:
   ```sh
   sudo systemctl start gpu-health-probe.service
   ```

## Escalation

Any DBE with `gpu_row_remap_failures > 0` → immediate decommission; escalate
to GPU infrastructure owner.

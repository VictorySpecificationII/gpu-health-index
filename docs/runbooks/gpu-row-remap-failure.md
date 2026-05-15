# GpuRowRemapFailure

**Severity:** critical  
**Fires when:** `gpu_row_remap_failures > 0` for 1 minute — at least one row
remapping failure has been recorded.

## Impact

Row remap failures are irreversible. The GPU has exhausted or failed its memory
repair mechanism. This device requires decommission evaluation.

## Diagnosis

1. **Confirm failure count:**
   ```sh
   curl -s localhost:9108/metrics | grep row_remap
   nvidia-smi -q | grep -i "remap"
   ```

2. **Is a reboot pending?**
   ```sh
   curl -s localhost:9108/metrics | grep pending_row_remap
   nvidia-smi -q | grep "Pending"
   ```

3. **Check for concurrent DBE events:**
   ```sh
   curl -s localhost:9108/metrics | grep -E "ecc_dbe|ecc_sbe"
   ```

4. **Check retired pages:**
   ```sh
   nvidia-smi -q | grep -A 5 "Retired Pages"
   ```

## Remediation

1. **Drain all workloads immediately.**
2. Reboot if pending retirement is flagged:
   ```sh
   sudo reboot
   ```
3. After reboot — re-check failure count. If non-zero: **decommission**.
   Row remap failures do not self-heal.
4. Document: serial, failure count, ECC history, retired page count, date.
   This record is required for warranty/RMA processing.

## False positive

None. Row remap failures reported by NVML are always real hardware events.

## Escalation

Any non-zero row remap failure count → escalate to GPU infrastructure owner
immediately for decommission scheduling and RMA initiation.

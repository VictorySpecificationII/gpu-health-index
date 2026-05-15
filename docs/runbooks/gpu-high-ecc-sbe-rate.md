# GpuHighEccSbeRate

**Severity:** warning  
**Fires when:** `gpu_ecc_sbe_rate_per_hour > 100` for 5 minutes — elevated
single-bit error rate over the scoring window.

## Impact

SBEs are correctable and do not cause data corruption by themselves. However,
a high SBE rate is a leading indicator of hardware degradation and increases
the probability of a future DBE event.

## Diagnosis

1. **Current SBE rate and aggregate count:**
   ```sh
   curl -s localhost:9108/metrics | grep -E "ecc_sbe|ecc_dbe"
   nvidia-smi -q -d ECC
   ```

2. **Is the SBE rate climbing or stable?**
   ```sh
   watch -n 60 "curl -s localhost:9108/metrics | grep ecc_sbe_rate"
   ```

3. **Check temperature — thermal stress drives SBE rate:**
   ```sh
   curl -s localhost:9108/metrics | grep -E "temp_p95|temp_celsius"
   nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader
   ```

4. **Has this GPU had prior DBE events?**
   ```sh
   curl -s localhost:9108/metrics | grep ecc_dbe_aggregate
   ```

## Remediation

- If SBE rate is stable (not climbing) and no prior DBEs: monitor; set a
  reminder to check aggregate count in 24 hours.
- If SBE rate is climbing or temperature is elevated: investigate cooling,
  reduce workload power limit temporarily:
  ```sh
  nvidia-smi -pl <limit_watts>
  ```
- If DBE events follow within 24 hours: follow [gpu-ecc-double-bit-error.md](gpu-ecc-double-bit-error.md).

## Escalation

SBE rate > 1000/hr or any DBE appears → escalate; likely precursor to hardware
failure.

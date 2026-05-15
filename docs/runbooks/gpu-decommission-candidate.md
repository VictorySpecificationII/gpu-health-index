# GpuDecommissionCandidate

**Severity:** critical  
**Fires when:** `gpu_health_class == 4` for 5 minutes — health score has been
below 50 for a sustained period.

## Impact

GPU is assessed as unfit for production workloads. The scoring model has
accumulated enough signal (ECC errors, retired pages, performance drop, thermal
excursions) to classify this device as a decommission candidate.

## Diagnosis

1. **Check the current score and which components are penalised:**
   ```sh
   curl -s localhost:9108/metrics | grep -E "health_score|health_class|ecc|retired|remap|probe|temp_p95"
   ```

2. **ECC aggregate counts (lifetime):**
   ```sh
   nvidia-smi -q -d ECC | grep -A 20 "GPU 000"
   ```

3. **Retired pages:**
   ```sh
   nvidia-smi -q | grep -A 5 "Retired Pages"
   ```

4. **Row remap failures:**
   ```sh
   nvidia-smi -q | grep -i "remap"
   ```

5. **Probe result age (is the score based on a stale probe?):**
   ```sh
   curl -s localhost:9108/metrics | grep -E "probe_result_stale|probe_available|baseline_age"
   ```

## Remediation

1. **Drain workloads** from this GPU immediately.
2. Document findings: serial, current score, ECC counts, retired pages,
   row remap failures, probe performance drop.
3. Run a fresh cuBLAS probe to confirm performance baseline:
   ```sh
   sudo systemctl start gpu-health-probe.service
   journalctl -u gpu-health-probe.service --no-pager
   ```
4. If score remains < 50 after fresh probe → **schedule hardware replacement**.
5. If score recovers (transient thermal event) → monitor; do not return to
   production without 24h stable score ≥ 70.

## False positive

Score can dip transiently if a long thermal violation or power capping event
fills the scoring window. Check `gpu_power_saturation_ratio` and
`gpu_temp_p95_celsius`. If thermal/power metrics are now normal and score is
recovering, watch for 30 minutes before resolving.

## Escalation

Row remap failures > 0 or DBE errors present → immediate decommission; escalate
to GPU infrastructure owner and data-centre ops for replacement.

# GPU Health Exporter — On-Call Runbook

Covers all 11 Prometheus alerting rules defined in
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

## GpuExporterDown

**Severity:** critical  
**Fires when:** `up{job="gpu-health"} == 0` for 1 minute — Prometheus cannot
scrape the exporter endpoint.

### Impact
All GPU health metrics are stale. `GpuUnavailable`, `GpuDcgmUnavailable`, and
all score-based alerts will not fire while the exporter is down. Blind spot on
all GPUs on this host.

### Diagnosis

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

### Remediation

| Root cause | Action |
|---|---|
| Service crashed (no driver change) | `sudo systemctl restart gpu-health.service` |
| Port conflict | Identify conflicting process: `ss -tlnp \| grep 9108`; resolve before restarting |
| Driver reload / nvidia-smi not responding | Reboot the host; re-enable service after reboot |
| file_sd.json missing | Restart service — it writes the file at startup |

### Escalation
Restart fails or recurs within 1 hour → escalate to GPU infrastructure owner.

---

## GpuUnavailable

**Severity:** critical  
**Fires when:** `gpu_available == 0` for 1 minute — the exporter has exceeded
its NVML error threshold for this GPU.

### Impact
NVML is no longer responding for this device. Health score is frozen; ECC,
temperature, and utilisation metrics are stale. Workloads on this GPU may be
affected.

### Diagnosis

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
   - **XID 48** — DBE / uncorrectable ECC error. See `GpuEccDoubleBitError`.
   - **XID 79** — GPU fell off the bus. Hardware fault; reboot required.
   - **XID 31** — GPU MMU fault. Drain workloads; investigate application.

4. **GPU fell off the bus (XID 79):**
   Drain all workloads on the host. Schedule reboot. After reboot check:
   ```sh
   nvidia-smi -q -d ECC
   nvidia-smi -q | grep "Retired Pages"
   ```

### Remediation

| Root cause | Action |
|---|---|
| Transient NVML error streak | `sudo systemctl restart gpu-health.service`; monitor `gpu_available` |
| XID 79 (GPU off bus) | Drain workloads → reboot → post-reboot ECC check |
| XID 48 (DBE) | Follow `GpuEccDoubleBitError` runbook |
| Driver wedge (nvidia-smi hangs) | Reboot; notify GPU infrastructure owner |

### Escalation
GPU does not recover after reboot → hardware replacement; escalate to
data-centre ops.

---

## GpuDcgmUnavailable

**Severity:** critical  
**Fires when:** `gpu_dcgm_available == 0` for 2 minutes — the exporter's DCGM
connection has failed or DCGM is not responding.

### Impact
Board power, total energy, memory bandwidth utilisation, and violation counters
are unavailable (`NaN` in metrics). The health score's DCGM-sourced components
are dropped. DCGM is operationally required on this fleet — this is not an
acceptable degraded mode.

### Diagnosis

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

### Remediation

| Root cause | Action |
|---|---|
| DCGM daemon stopped | `sudo systemctl restart nvidia-dcgm.service`; verify `dcgmi discovery -l` |
| DCGM daemon not starting | Check driver compatibility: `dcgmi --version` vs driver version |
| Exporter lost connection after DCGM restart | `sudo systemctl restart gpu-health.service` |
| Fabric Manager not running (NVSwitch systems) | `sudo systemctl start nvidia-fabricmanager.service` |

### Escalation
DCGM fails to start after driver check → escalate to GPU infrastructure owner.

---

## GpuDecommissionCandidate

**Severity:** critical  
**Fires when:** `gpu_health_class == 4` for 5 minutes — health score has been
below 50 for a sustained period.

### Impact
GPU is assessed as unfit for production workloads. The scoring model has
accumulated enough signal (ECC errors, retired pages, performance drop, thermal
excursions) to classify this device as a decommission candidate.

### Diagnosis

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

### Remediation

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

### False positive
Score can dip transiently if a long thermal violation or power capping event
fills the scoring window. Check `gpu_power_saturation_ratio` and
`gpu_temp_p95_celsius`. If thermal/power metrics are now normal and score is
recovering, watch for 30 minutes before resolving.

### Escalation
Row remap failures > 0 or DBE errors present → immediate decommission; escalate
to GPU infrastructure owner and data-centre ops for replacement.

---

## GpuDegrading

**Severity:** warning  
**Fires when:** `gpu_health_class == 3` for 5 minutes — health score between
50 and 70.

### Impact
GPU is degrading but not yet a decommission candidate. Workloads can continue
but the device requires active monitoring. May progress to class 4 within hours
or days.

### Diagnosis

Same steps as `GpuDecommissionCandidate`. Key question: **what is driving the
score down and is it trending further?**

```sh
# Score trend — watch over 10 minutes
watch -n 30 "curl -s localhost:9108/metrics | grep health_score"
```

### Remediation

- Do not drain immediately unless score is falling rapidly toward 50.
- Schedule a fresh probe run within 24 hours:
  ```sh
  sudo systemctl start gpu-health-probe.service
  ```
- Increase monitoring frequency; set a calendar reminder to recheck in 4 hours.
- If ECC SBE rate is elevated (see `GpuHighEccSbeRate`), track whether it
  converts to DBE events.

### Escalation
Score crosses 50 → treat as `GpuDecommissionCandidate`.

---

## GpuTelemetryIncomplete

**Severity:** warning  
**Fires when:** `gpu_telemetry_ok == 0` for 5 minutes — fewer samples than
expected in the scoring window. Score is suppressed (class 0 / N/A).

### Impact
Health score is not being computed. This GPU will not trigger score-based
alerts (`GpuDegrading`, `GpuDecommissionCandidate`) until telemetry recovers.

### Diagnosis

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

### Remediation

| Root cause | Action |
|---|---|
| Poll thread stalled | `sudo systemctl restart gpu-health.service` |
| GPU disappeared (`gpu_present == 0`) | Follow `GpuUnavailable` runbook |
| Exporter just started | Wait one full `window_s` (default 300s) for window to fill |

### Escalation
Poll thread stall recurs after restart → possible kernel or driver issue;
escalate to GPU infrastructure owner.

---

## GpuEccDoubleBitError

**Severity:** critical  
**Fires when:** `gpu_ecc_dbe_in_window == 1` for 1 minute — at least one ECC
double-bit error delta was observed in the scoring window.

### Impact
Uncorrectable memory corruption has occurred. Any workload currently running
on this GPU may have produced incorrect results. Immediate action required.

### Diagnosis

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

### Remediation

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

### Escalation
Any DBE with `gpu_row_remap_failures > 0` → immediate decommission; escalate
to GPU infrastructure owner.

---

## GpuHighEccSbeRate

**Severity:** warning  
**Fires when:** `gpu_ecc_sbe_rate_per_hour > 100` for 5 minutes — elevated
single-bit error rate over the scoring window.

### Impact
SBEs are correctable and do not cause data corruption by themselves. However,
a high SBE rate is a leading indicator of hardware degradation and increases
the probability of a future DBE event.

### Diagnosis

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

### Remediation

- If SBE rate is stable (not climbing) and no prior DBEs: monitor; set a
  reminder to check aggregate count in 24 hours.
- If SBE rate is climbing or temperature is elevated: investigate cooling,
  reduce workload power limit temporarily:
  ```sh
  nvidia-smi -pl <limit_watts>
  ```
- If DBE events follow within 24 hours: follow `GpuEccDoubleBitError` runbook.

### Escalation
SBE rate > 1000/hr or any DBE appears → escalate; likely precursor to hardware
failure.

---

## GpuRowRemapFailure

**Severity:** critical  
**Fires when:** `gpu_row_remap_failures > 0` for 1 minute — at least one row
remapping failure has been recorded.

### Impact
Row remap failures are irreversible. The GPU has exhausted or failed its memory
repair mechanism. This device requires decommission evaluation.

### Diagnosis

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

### Remediation

1. **Drain all workloads immediately.**
2. Reboot if pending retirement is flagged:
   ```sh
   sudo reboot
   ```
3. After reboot — re-check failure count. If non-zero: **decommission**.
   Row remap failures do not self-heal.
4. Document: serial, failure count, ECC history, retired page count, date.
   This record is required for warranty/RMA processing.

### False positive
None. Row remap failures reported by NVML are always real hardware events.

### Escalation
Any non-zero row remap failure count → escalate to GPU infrastructure owner
immediately for decommission scheduling and RMA initiation.

---

## GpuPcieLinkDegraded

**Severity:** warning  
**Fires when:** `gpu_pcie_link_gen < gpu_pcie_link_gen_max` or
`gpu_pcie_link_width < gpu_pcie_link_width_max` for 2 minutes — the GPU is
negotiating a PCIe link below its maximum capability.

### Impact
Reduced host-to-GPU memory bandwidth. For memory-bandwidth-bound workloads
(LLM inference, large GEMM) this can reduce throughput by up to 30% depending
on how many generations or lanes are lost.

### Diagnosis

1. **Current vs maximum link state:**
   ```sh
   curl -s localhost:9108/metrics | grep pcie_link
   nvidia-smi -q | grep -A 5 "PCIe"
   ```

2. **Is this consistent across reboots or did it change recently?**
   Check Prometheus history or review recent changes (driver update, physical
   work in the rack).

3. **Other GPUs on the same host:**
   ```sh
   nvidia-smi -q | grep -A 5 "PCIe" | grep -E "Gen|Width"
   ```
   If all GPUs are degraded → likely a BIOS or platform issue, not a single
   card fault.

4. **Check BIOS PCIe settings** (requires console/IPMI access): confirm the
   slot is configured for the expected generation (Gen 4/5) and width (x16).

### Remediation

| Root cause | Action |
|---|---|
| Loose card seating | Power down host; reseat GPU; power on |
| BIOS misconfiguration | Set PCIe slot to Gen 5 x16 (or platform max) in BIOS |
| Platform downgrade after BIOS update | Revert BIOS or apply vendor fix |
| Thermal throttling on the link | Check chassis airflow; reduce ambient temperature |

After any physical intervention:
```sh
sudo reboot
curl -s localhost:9108/metrics | grep pcie_link
```

### Escalation
Link degradation persists after reseating and BIOS check → hardware fault;
escalate to data-centre ops for slot/riser inspection.

---

## GpuProbeResultStale

**Severity:** warning  
**Fires when:** `gpu_probe_result_stale == 1` for 25 hours — the cuBLAS probe
result file is older than the configured TTL.

### Impact
The performance/Watt component of the health score is dropped. The score may
be artificially high (masking a performance regression) until the probe runs.
`GpuDecommissionCandidate` could fail to fire if the perf drop is the primary
degradation signal.

### Diagnosis

1. **Is the probe timer active?**
   ```sh
   systemctl status gpu-health-probe.timer
   systemctl list-timers gpu-health-probe.timer
   ```

2. **When did the last probe run?**
   ```sh
   journalctl -u gpu-health-probe.service --since "2 days ago" --no-pager | tail -30
   ```

3. **Is the probe binary installed?**
   ```sh
   which gpu-health-probe
   ls -la /usr/local/bin/gpu-health-probe
   ```

4. **Is the state directory writable?**
   ```sh
   ls -la /var/run/gpu-health/
   ```

5. **Did the last probe run fail?**
   ```sh
   journalctl -u gpu-health-probe.service -n 50 --no-pager
   ```

### Remediation

| Root cause | Action |
|---|---|
| Timer disabled | `sudo systemctl enable --now gpu-health-probe.timer` |
| Probe binary missing | `make -C probe && sudo make -C probe install PREFIX=/usr/local` |
| Last probe failed (CUDA error) | Check logs; run manually to get live error output (see below) |
| State directory missing | `sudo mkdir -p /var/run/gpu-health && sudo chown gpu-health /var/run/gpu-health` |

**Force an immediate probe run:**
```sh
sudo systemctl start gpu-health-probe.service
journalctl -u gpu-health-probe.service -f
```

After a successful run:
```sh
curl -s localhost:9108/metrics | grep probe_result_stale
# Expect: gpu_probe_result_stale{serial="..."} 0
```

### False positive
If the exporter was just installed and the probe has never run, this alert will
fire after 25 hours. Run the probe once manually to establish the first result.

### Escalation
Probe fails repeatedly with CUDA errors while `nvidia-smi` shows the GPU
healthy → possible CUDA toolkit version mismatch; escalate to GPU
infrastructure owner.

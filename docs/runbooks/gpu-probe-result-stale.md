# GpuProbeResultStale

**Severity:** warning  
**Fires when:** `gpu_probe_result_stale == 1` for 25 hours — the cuBLAS probe
result file is older than the configured TTL.

## Impact

The performance/Watt component of the health score is dropped. The score may
be artificially high (masking a performance regression) until the probe runs.
`GpuDecommissionCandidate` could fail to fire if the perf drop is the primary
degradation signal.

## Diagnosis

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

## Remediation

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

## False positive

If the exporter was just installed and the probe has never run, this alert will
fire after 25 hours. Run the probe once manually to establish the first result.

## Escalation

Probe fails repeatedly with CUDA errors while `nvidia-smi` shows the GPU
healthy → possible CUDA toolkit version mismatch; escalate to GPU
infrastructure owner.

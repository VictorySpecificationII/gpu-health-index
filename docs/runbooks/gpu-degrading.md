# GpuDegrading

**Severity:** warning  
**Fires when:** `gpu_health_class == 3` for 5 minutes — health score between
50 and 70.

## Impact

GPU is degrading but not yet a decommission candidate. Workloads can continue
but the device requires active monitoring. May progress to class 4 within hours
or days.

## Diagnosis

Same steps as [gpu-decommission-candidate.md](gpu-decommission-candidate.md).
Key question: **what is driving the score down and is it trending further?**

```sh
# Score trend — watch over 10 minutes
watch -n 30 "curl -s localhost:9108/metrics | grep health_score"
```

## Remediation

- Do not drain immediately unless score is falling rapidly toward 50.
- Schedule a fresh probe run within 24 hours:
  ```sh
  sudo systemctl start gpu-health-probe.service
  ```
- Increase monitoring frequency; set a calendar reminder to recheck in 4 hours.
- If ECC SBE rate is elevated (see [gpu-high-ecc-sbe-rate.md](gpu-high-ecc-sbe-rate.md)),
  track whether it converts to DBE events.

## Escalation

Score crosses 50 → treat as [GpuDecommissionCandidate](gpu-decommission-candidate.md).

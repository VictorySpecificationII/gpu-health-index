# GpuPcieLinkDegraded

**Severity:** warning  
**Fires when:** `gpu_pcie_link_gen < gpu_pcie_link_gen_max` or
`gpu_pcie_link_width < gpu_pcie_link_width_max` for 2 minutes — the GPU is
negotiating a PCIe link below its maximum capability.

## Impact

Reduced host-to-GPU memory bandwidth. For memory-bandwidth-bound workloads
(LLM inference, large GEMM) this can reduce throughput by up to 30% depending
on how many generations or lanes are lost.

## Diagnosis

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

## Remediation

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

## Escalation

Link degradation persists after reseating and BIOS check → hardware fault;
escalate to data-centre ops for slot/riser inspection.

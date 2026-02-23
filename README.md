# Telemetry-Driven GPU Health Assessment  
## A Baseline and Drift Methodology

---

## Abstract

This project defines a structured methodology for assessing GPU health using telemetry-derived metrics and controlled stress profiles. Rather than relying on snapshot inspection, the approach establishes per-device performance baselines and detects degradation through temporal drift analysis. The output is an interpretable health score intended to support maintenance, investigation, and decommissioning decisions.

Telemetry collection is implemented using both NVIDIA Management Library (NVML) and Data Center GPU Manager (DCGM), enabling compatibility across standalone and fleet-scale environments.

Validation is performed on a single NVIDIA H200 instance for baseline characterization. Degradation behavior is demonstrated using replay-mode datasets simulating common failure patterns (e.g., ECC accumulation, throttling drift, performance-per-watt degradation).

---

## 1. Motivation

GPU failures in production environments are costly and rarely instantaneous. Degradation is often preceded by detectable signals such as:

- Increasing ECC error counts  
- Sustained thermal stress  
- Clock instability under steady load  
- Elevated throttling frequency  
- Declining performance-per-watt  

Operational practices often rely on static thresholds or manual inspection. These approaches lack:

- Per-device normalization  
- Temporal trend awareness  
- Reproducible stress conditions  
- Quantified decision criteria  

This project proposes a repeatable methodology emphasizing:

- Controlled baseline acquisition  
- Temporal drift detection  
- Explainable scoring logic  
- Clear classification thresholds  

---

## 2. System Model

A GPU is modeled as a coupled thermodynamic and electrical system operating within defined envelopes of:

- Power delivery  
- Thermal dissipation  
- Clock stability  
- Memory integrity  

Telemetry signals are treated as observable proxies for internal state.

Health is defined not as an absolute constant, but as deviation from stable baseline behavior under controlled workload conditions.

### Assumptions

1. Telemetry contains noise and must be interpreted temporally.  
2. Degradation is often gradual rather than binary.  
3. Environmental factors (cooling, ambient temperature, workload type) influence observable metrics.  
4. Interpretability is prioritized over black-box predictive modeling (v0 model).

---

## 3. Experimental Methodology

### 3.1 Baseline Acquisition

A structured test sequence establishes per-device reference behavior:

1. **Idle Phase**  
   Capture steady-state baseline telemetry.

2. **Compute-Bound Stress**  
   Sustained arithmetic workload to evaluate:
   - SM clock stability  
   - Power draw consistency  
   - Thermal response  

3. **Memory-Bound Stress**  
   Memory-intensive workload to evaluate:
   - HBM utilization  
   - Memory clock behavior  
   - Bandwidth stability  

4. **Thermal Soak**  
   Sustained high-load period to assess:
   - Long-term thermal equilibrium  
   - Throttling onset  
   - Power envelope limits  

5. **Cooldown Phase**  
   Observe recovery behavior and thermal decay characteristics.

Telemetry is sampled at fixed intervals (default: 1 Hz).

---

### 3.2 Telemetry Collection

Telemetry is collected using:

- **NVML** for per-device metric access and low-level state inspection  
- **DCGM** for data center–grade monitoring and fleet-aligned metric coverage  

Metrics collected include:

- GPU temperature  
- Power draw and power limit  
- SM clock frequency  
- Memory clock frequency  
- GPU utilization  
- Memory utilization  
- Memory usage  
- Performance state (P-state)  
- ECC error counters  
- Throttle indicators  

Metric availability depends on GPU model and driver support.

---

## 4. Health Scoring Model

Health is expressed as a normalized score:

```bash
Health Score = 100 − Σ(penalties)
```


Each penalty corresponds to a physical reliability signal.

### 4.1 Penalty Categories

**Thermal Headroom Penalty**  
Applied when sustained load temperature approaches defined safe limits.

**Clock Stability Penalty**  
Based on increased variance of SM clock under steady load.

**Performance-per-Watt Penalty**  
Computed as deviation from baseline throughput-to-power ratio.

**Throttling Penalty**  
Applied when throttle frequency or duration increases beyond baseline.

**ECC Accumulation Penalty**  
Rate-based penalty for increasing correctable or uncorrectable errors.

Penalties are deterministic and interpretable.

---

## 5. Drift Detection Strategy

Single metric spikes are not sufficient indicators of degradation.

Temporal analysis includes:

- Rolling mean and standard deviation  
- Z-score anomaly detection  
- Linear trend slope analysis  
- Rate-of-change monitoring  

Degradation is defined as sustained deviation rather than isolated anomaly.

---

## 6. Classification Thresholds

| Score Range | Classification |
|-------------|---------------|
| 85–100      | Healthy       |
| 70–85       | Monitor       |
| 50–70       | Degrading     |
| <50         | Decommission Candidate |

Thresholds are configurable and intended for calibration in production environments.

---

## 7. Validation

### 7.1 Live Validation

Telemetry capture and baseline characterization validated on:

- NVIDIA H200 (single instance)  
- Controlled stress phases  

This validates telemetry pipeline, baseline methodology, and scoring logic.

### 7.2 Replay Mode

Synthetic degradation datasets simulate:

- Gradual ECC accumulation  
- Increased throttling frequency  
- Declining performance-per-watt  

Replay mode enables deterministic testing of drift detection and classification logic without requiring long-term aging data.

---

## 8. Production Considerations

Future production-scale extensions include:

- Fleet-wide DCGM integration  
- Per-GPU-model baseline distributions  
- Automated threshold recalibration  
- Correlation with RMA/failure labels  
- Economic decommission modeling  

---

## 9. Limitations

- Single-device validation (no fleet-scale calibration yet)  
- No long-term aging dataset  
- Environmental coupling not modeled  
- Firmware-dependent metric variability  
- Deterministic scoring (no predictive ML in current version)

---

## 10. Conclusion

This project defines a structured, reproducible methodology for GPU health assessment grounded in:

- Controlled stress testing  
- Baseline normalization  
- Temporal drift detection  
- Interpretable scoring  

The approach provides a foundation for scalable, fleet-wide GPU reliability modeling.

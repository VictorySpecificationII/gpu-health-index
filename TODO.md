# GPU Health Exporter — TODO

## Phase 1 — remaining for production deployment

### Code gaps
> No design decisions needed — documented in source comments.

- [x] `gpu_snapshot_t` missing `collector_errors_total` — add field to `types.h`, wire from `gpu_ctx_t.collector_errors_total` in `snapshot.c`, render in `http.c`
- [x] ECC volatile counters (SBE/DBE volatile) forwarded in snapshot and exposed as `gpu_ecc_sbe_volatile_total` / `gpu_ecc_dbe_volatile_total`
- [x] `gpu_dcgm_available` in `http.c` is inferred from `isnan(mem_bw_util_pct)` — correctness gap: a field that reads NaN for other reasons would silently suppress the DCGM alert signal. Add explicit `dcgm_available` field to `gpu_snapshot_t`; set it in `snapshot_update()` from the `dcgm_available` param that is currently discarded. DCGM is operationally required on this fleet — this metric must be trustworthy.

### Security hardening

- [x] `procpriv_child_setup()` — `capset()` to all-zeros + seccomp whitelist: `accept4`, `read`, `write`, `close`, `select`, `socket`, `bind`, `listen`, `sendto`, `recvfrom`, `sigaction`, `exit_group`
- [x] `procpriv_parent_setup()` — `capset()` drop to minimal set after NVML file descriptors are already open

### Optional TLS
- [x] `http.c` — `WITH_TLS=1` build path: mbedTLS server setup, cert/key load from config (`tls_cert_path`, `tls_key_path`), wrap each accepted fd before request dispatch
  - Local: TLS handshake + full request flow validated via `make WITH_TLS=1 build/test_http` (no GPU needed — test harness synthesises IPC)
  - GPU node: validate that seccomp filter permits all handshake syscalls under the real `procpriv_child_setup()` path

### Deployment artifacts
- [x] `deploy/gpu-health.service` — systemd unit (bare metal)
- [x] `deploy/gpu-health.conf.example` — fully annotated config file
- [x] `deploy/gpu-health-probe.service` + `deploy/gpu-health-probe.timer` — periodic cuBLAS probe (daily, per-GPU)
- [ ] `deploy/k8s/daemonset.yaml`
- [ ] `deploy/k8s/configmap-baseline.yaml`
- [ ] `deploy/k8s/servicemonitor.yaml`
- [ ] `deploy/k8s/rbac.yaml`
- [ ] `deploy/k8s/Chart.yaml` — Helm chart root
- [x] Prometheus `file_sd` entry written at startup (bare metal path only — see DESIGN.md §2.10)

### Tests
- [x] `tests/test_http.c` — `http.c` has no unit tests; cover `render_metrics` output format, `/ready` and `/live` response codes, NaN field handling

---

## Phase 1.5 — bare metal hardening (before K8s)

### Alerting
- [ ] Prometheus alerting rules — `gpu_health_class=4` (decommission candidate), `gpu_dcgm_available=0`, `gpu_available=0`, `gpu_telemetry_ok=0`, `gpu_ecc_dbe_in_window=1`, `gpu_probe_result_stale=1`
- [ ] Grafana alert annotations or contact point wiring (optional — Prometheus Alertmanager is sufficient)

### Baseline history
- [ ] Append-only history record per GPU — each probe run appended to `{serial}.history` (timestamp, driver, perf_w_mean, sample_count); never read by the exporter, consumed by Phase 2 financial layer
- [ ] Decide on history storage backend for bare metal (local append file vs. external store)

### Security
- [ ] HTTP parser audit — review `read_request_line` against malformed/oversized input; confirm no path traversal or header injection surface

### Operational
- [ ] Runbook — what on-call does when `gpu_health_class=4` fires, DCGM drops, probe goes stale, ECC DBE appears
- [ ] Log rotation — confirm `journald` RateLimitBurst covers poll-rate log volume; document if `journald` is not used

---

## Phase 2 — financial layer
> Separate system. Consumers of the exporter's outputs. Not blocking Phase 1.

- [x] `probe/gpu_health_probe.cu` — cuBLAS BF16 GEMM probe binary
- [x] `probe/Makefile`
- [ ] Assessment report generator (structured JSON + human-readable format)
- [ ] Lifetime degradation record accumulator (ECC aggregate, retired pages, row remap history over GPU's monitored lifetime)
- [ ] NVIDIA attestation integration (H100/H200 Confidential Computing)
- [ ] Financial model input pipeline

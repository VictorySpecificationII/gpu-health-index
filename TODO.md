# GPU Health Exporter — TODO

## Phase 1 — remaining for production deployment

### Code gaps
> No design decisions needed — documented in source comments.

- [x] `gpu_snapshot_t` missing `collector_errors_total` — add field to `types.h`, wire from `gpu_ctx_t.collector_errors_total` in `snapshot.c`, render in `http.c`
- [ ] ECC volatile counters (SBE/DBE volatile) not forwarded in snapshot — only aggregate + rate available; add fields to `gpu_snapshot_t` if per-scrape volatile count is needed
- [x] `gpu_dcgm_available` in `http.c` is inferred from `isnan(mem_bw_util_pct)` — correctness gap: a field that reads NaN for other reasons would silently suppress the DCGM alert signal. Add explicit `dcgm_available` field to `gpu_snapshot_t`; set it in `snapshot_update()` from the `dcgm_available` param that is currently discarded. DCGM is operationally required on this fleet — this metric must be trustworthy.

### Security hardening
> `procpriv.c` is stubs only (`PR_SET_NO_NEW_PRIVS`). Full implementation below.

- [ ] `procpriv_child_setup()` — `capset()` to all-zeros + seccomp whitelist: `accept4`, `read`, `write`, `close`, `select`, `socket`, `bind`, `listen`, `sendto`, `recvfrom`, `sigaction`, `exit_group`
- [ ] `procpriv_parent_setup()` — `capset()` drop to minimal set after NVML file descriptors are already open

### Optional TLS
- [ ] `http.c` — `WITH_TLS=1` build path: mbedTLS server setup, cert/key load from config (`tls_cert_path`, `tls_key_path`), wrap each accepted fd before request dispatch

### Deployment artifacts
- [x] `deploy/gpu-health.service` — systemd unit (bare metal)
- [x] `deploy/gpu-health.conf.example` — fully annotated config file
- [ ] `deploy/k8s/daemonset.yaml`
- [ ] `deploy/k8s/configmap-baseline.yaml`
- [ ] `deploy/k8s/servicemonitor.yaml`
- [ ] `deploy/k8s/rbac.yaml`
- [ ] `deploy/k8s/Chart.yaml` — Helm chart root
- [ ] Prometheus `file_sd` entry written at startup (bare metal path only — see DESIGN.md §2.10)

### Tests
- [x] `tests/test_http.c` — `http.c` has no unit tests; cover `render_metrics` output format, `/ready` and `/live` response codes, NaN field handling

---

## Phase 2 — financial layer
> Separate system. Consumers of the exporter's outputs. Not blocking Phase 1.

- [ ] `probe/gpu_health_probe.cu` — cuBLAS BF16 GEMM probe binary
- [ ] `probe/Makefile`
- [ ] Assessment report generator (structured JSON + human-readable format)
- [ ] Lifetime degradation record accumulator (ECC aggregate, retired pages, row remap history over GPU's monitored lifetime)
- [ ] NVIDIA attestation integration (H100/H200 Confidential Computing)
- [ ] Financial model input pipeline

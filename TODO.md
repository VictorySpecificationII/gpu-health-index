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
  - GPU node: validated on H200 (serial 1653724086028, driver 580.126.09) — TLSv1.2, ECDHE-RSA-CHACHA20-POLY1305, secp521r1, seccomp filter active throughout

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
- [x] Prometheus alerting rules — `deploy/monitoring/prometheus/alerts.yml`: GpuExporterDown, GpuUnavailable, GpuDcgmUnavailable, GpuDecommissionCandidate, GpuDegrading, GpuTelemetryIncomplete, GpuEccDoubleBitError, GpuHighEccSbeRate, GpuRowRemapFailure, GpuPcieLinkDegraded, GpuProbeResultStale
- [x] Alertmanager service added to docker-compose; prometheus.yml wired to localhost:9093; alertmanager.yml has stub receiver with Slack/email/PagerDuty/webhook templates and inhibit rule (critical suppresses warning for same GPU); Grafana provisioned with Alertmanager data source

### Baseline history
- [x] Per-probe JSON record written to S3 — `deploy/gpu-health-s3-writer`; keyed `{prefix}/{serial}/{probe_timestamp}.json`; called from probe-run, non-fatal on failure
- [x] Writer component — reads `{serial}.probe`, builds JSON, uploads via `aws s3api put-object`; no-op if `S3_BUCKET` not set; config in `/etc/gpu-health/s3.conf`
- [x] Local S3 for dev/test — LocalStack added to docker-compose (`SERVICES=s3`, port 4566)
- [ ] Local fallback — if S3 not configured, append to `{serial}.history` in state_dir for bare metal single-node setups

### Security
- [x] HTTP parser audit — no vulnerabilities found; buffer bounds tight (REQ_BUF_SIZE=4096), slow loris mitigated by SO_RCVTIMEO=5s, no FS access in request handling, response headers use only static strings; `g_running` corrected to `volatile sig_atomic_t`

### Operational
- [x] Runbook — `docs/runbooks/`: one file per alert, index at `docs/runbooks/README.md`; diagnosis steps, remediation table, false positives, escalation criteria
- [x] Log rotation — journald handles rotation; default RateLimitBurst (10000/30s) not a concern; steady-state INFO log volume is near-zero; documented in README

---

## Phase 2 — financial layer
> Separate system. Consumers of the exporter's outputs. Not blocking Phase 1.

- [x] `probe/gpu_health_probe.cu` — cuBLAS BF16 GEMM probe binary
- [x] `probe/Makefile`
- [ ] Assessment report generator (structured JSON + human-readable format)
- [ ] Lifetime degradation record accumulator (ECC aggregate, retired pages, row remap history over GPU's monitored lifetime)
- [ ] NVIDIA attestation integration (H100/H200 Confidential Computing)
- [ ] Financial model input pipeline

# Architecture Decision Records

Decisions are recorded in [Michael Nygard format](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions).
Each record captures the context, the decision, and its consequences at the time it was made.
Superseded records are kept for audit trail.

---

## ADR-001: Language choice — C

**Status:** Accepted

### Context

The v0 prototype is Python. It works for research but is too heavy for fleet-scale deployment:
process startup latency, interpreter overhead, and dependency management are all friction at
thousands of multi-GPU hosts. The target is one monitoring binary per node, permanently resident,
with minimal resource footprint.

Candidates considered: Go, Rust, C.

Go: acceptable performance, but the runtime adds ~8 MB of baseline RSS and the garbage collector
introduces latency spikes that complicate tight poll loops. CGo bridging to NVML/DCGM adds
complexity and breaks Go's static linking guarantees.

Rust: correct choice for greenfield systems software; however, the NVML/DCGM FFI surface is large
and the safe wrapper ecosystem is immature. Build toolchain complexity increases the operational
cost for an HPC environment that may not have a Rust toolchain in the golden image.

C: zero mandatory non-system dependencies beyond libc. dlopen/dlsym for NVML/DCGM is idiomatic C.
The polling loop, ring buffer, and scoring arithmetic are straightforward C with no language-level
impedance mismatch. The security model (fork, capset, seccomp-BPF) is a POSIX/Linux API — C is
the native language of that API.

### Decision

Implement in C11 (`-std=c11`), targeting GCC 9+ and Clang 10+. Statically link everything except
the dlopen'd NVML and DCGM libraries. Compile once per architecture, deploy anywhere with a
compatible glibc.

### Consequences

- Binary is ~86 KB. No runtime dependency resolution on target nodes.
- All NVML/DCGM type definitions must be mirrored in-tree — we do not include NVIDIA headers.
  This is maintenance work but removes the CUDA SDK as a build dependency.
- Manual memory management required. Mitigated by building with ASan/UBSan in debug mode and
  scoping dynamic allocation to startup only.
- MISRA C 2012 compliance is not a goal; the standard targets safety-critical embedded systems,
  not financial infrastructure software. Standard C idioms (early return, volatiles as booleans,
  dlopen pointer casts) are acceptable.

---

## ADR-002: Process topology — one process per node, one poll thread per GPU

**Status:** Accepted

### Context

Two structural alternatives:

**One process per GPU:** natural blast radius isolation; a wedged poll loop cannot affect other
GPUs. However: one systemd unit per GPU per node, one port per GPU, one file_sd entry per GPU,
one set of log streams per GPU. Operationally expensive at scale. Kubernetes makes this worse —
a pod-per-GPU DaemonSet requires affinity rules and GPU resource allocation that fights the
scheduler.

**One process per node, one poll thread per GPU:** single systemd unit, single port 9108, single
log stream, single Prometheus scrape target per node. Blast radius contained by design: each GPU
has its own ring buffer, state struct, snapshot struct, and mutex. A wedged poll thread stales
only its own GPU's metrics; other GPUs are unaffected. Parent monitors threads and can detect
hangs via poll timestamp staleness.

### Decision

One process per node. One poll thread per GPU. Each GPU context is fully independent within the
process.

### Consequences

- Single port 9108 per node. One file_sd entry (bare metal) or one ServiceMonitor target
  (Kubernetes) per node.
- A poll thread hang does not block other GPUs, but it does not self-recover without watchdog
  logic. Watchdog is implemented via poll timestamp staleness detection in the parent loop.
- If the process crashes, all GPU metrics go stale simultaneously. This is acceptable because the
  most likely cause of a process crash is a driver-level event that affects all GPUs on the node
  anyway. Systemd restarts the unit.

---

## ADR-003: NVML and DCGM loaded via dlopen, not linked

**Status:** Accepted

### Context

NVML (`libnvidia-ml.so`) and DCGM (`libdcgm.so`) are NVIDIA libraries not present on all systems
at build time. Hard-linking either library would make the binary fail to start on any node where
the library is absent or at a mismatched path, even if the GPU hardware is present.

### Decision

Load both libraries at runtime via `dlopen`/`dlsym`. All NVML and DCGM function pointers are
stored in vtable structs (`nvml_vtable_t`, `dcgm_vtable_t`). NVML is required; the binary exits
at startup if NVML is unavailable. DCGM is optional; if absent, DCGM-sourced fields emit NaN and
`gpu_dcgm_available` is set to 0.

All NVML and DCGM type definitions are mirrored in-tree. We do not depend on NVIDIA's header
files at build time.

The vtable pattern has a secondary benefit: in tests, the vtable is populated with fake functions
rather than real library calls. This allows the full test suite to run without GPU hardware or
driver installation.

### Consequences

- Build has no dependency on the CUDA SDK, DCGM headers, or NVIDIA driver packages.
- ABI compatibility must be maintained manually. NVML is additive (new functions, new fields) so
  this is low-risk in practice. Blackwell support comes naturally as NVML extends Ampere.
- DCGM is operationally required on the target fleet. The NVML-only fallback path exists for
  robustness but `gpu_dcgm_available 0` is an anomaly worth alerting on, not an acceptable
  steady state.

---

## ADR-004: Serial number as primary GPU identifier

**Status:** Accepted

### Context

GPU identity is needed to correlate health data across time — the ring buffer, baselines, probes,
and state files all need a stable key. Candidates: GPU index (PCIe slot), UUID, serial number.

GPU index: changes if GPUs are added, removed, or the node reboots with different slot
enumeration order. Unusable for long-term tracking.

UUID (`nvmlDeviceGetUUID()`): logically stable, but can change after firmware update, infoROM
reflash, or RMA board swap. NVIDIA documents this behavior. A UUID change after a maintenance
event would silently break the continuity of health tracking for that GPU without any warning.

Serial number (`nvmlDeviceGetSerial()`): burned into hardware at manufacturing. Survives infoROM
reflash, driver update, and most RMA procedures (unless the board is physically replaced, which
is exactly when you want a new health record). Stable across reboots, slot changes, and UUID
resets.

### Decision

Serial number is the primary GPU identifier. It is the baseline file key, the state file key, the
primary Prometheus label on all hot metrics, and the IPC identity. UUID is retained in the
`gpu_info{...}` gauge for cross-referencing with existing tooling (nvidia-smi, dcgm-exporter,
cluster managers). Identity source is exposed as a metric (`gpu_identity_source`) so operators
can detect the rare case where serial is unavailable and UUID fallback is in use.

### Consequences

- Baselines survive GPU moves between PCIe slots and driver reinstallation. No re-provisioning
  required after routine maintenance.
- If a serial number is genuinely unavailable (some virtual GPUs, some older firmware), the
  fallback hierarchy is: serial → UUID → startup abort. UUID fallback is logged as a warning.
- Physical board replacement correctly produces a new health record with no baseline (expected).

---

## ADR-005: Metric label design — serial only on hot metrics, metadata in gpu_info gauge

**Status:** Accepted

### Context

GPU metadata (model name, UUID, PCIe address, driver version, GPU index) is tempting to put on
every metric as labels. This is the pattern used by node_exporter and dcgm-exporter.

Problems with high-cardinality label sets on hot metrics:
- Every unique label combination is a separate Prometheus time series. A 8-GPU node with 55
  metrics and 5 labels per metric creates 8 × 55 × (label cardinality) series.
- Driver updates, UUID changes, or model name normalisation changes cause series discontinuity —
  existing recording rules and alerts break silently.
- Queries that do not need metadata pay the cardinality cost anyway.

### Decision

Serial number is the only label on all hot metrics (score, thermal, power, ECC, clocks, etc.).
All metadata is in a single `gpu_info{serial, uuid, gpu_model, gpu_index, driver_version, ...} 1`
gauge. PromQL join via `* on(serial) group_left(gpu_model, ...) gpu_info` when metadata is needed
in a query.

### Consequences

- Cardinality is minimal. 8 GPUs × 55 metrics = 440 series regardless of label set size.
- Driver update or UUID reset does not break any existing series or alert. Only `gpu_info` changes.
- Queries that need metadata require an explicit join. This is slightly more verbose but makes the
  cardinality cost explicit and intentional.

---

## ADR-006: Scoring at the edge — inline in the exporter

**Status:** Accepted

### Context

Two structural approaches to GPU health scoring:

**Downstream scoring:** treat the exporter as a dumb metric emitter (like dcgm-exporter), and
compute health scores in Prometheus recording rules, a Ruler, or a separate scoring service.
Advantages: separation of concerns, faster scoring model iteration, centralised threshold
management, transparent logic visible in PromQL.

**Edge scoring:** compute health scores inside the exporter on every poll cycle and emit the score
as a metric. Advantages: access to internal state not representable in Prometheus, correctness
in failure modes.

Three properties of the scoring model make downstream computation structurally incorrect:

1. **Telemetry completeness gate.** The gate checks that at least N samples were collected in the
   300-second ring buffer window, and that the median inter-sample step does not exceed a
   threshold. This requires knowledge of how many samples the collector *actually wrote*, which
   is ring buffer state — not Prometheus scrape state. A gap in TSDB could be a scrape failure,
   a network blip, or a dead GPU. The exporter knows which because it is counting its own ring
   fills. Downstream, you cannot recover this distinction from TSDB alone. The gate's entire
   purpose is to return N/A rather than a misleading Healthy result when data is sparse — doing
   this correctly downstream is not practically possible.

2. **Baseline access.** The perf/W baseline is a per-GPU flat file on the host, keyed by serial.
   It is not natural Prometheus data. Getting it downstream requires either emitting it as a
   metric (then joining in PromQL across time series, which is fragile and defeats the purpose of
   keeping it out of the hot path), or building a separate pipeline that has access to both
   Prometheus and the baseline file tree. The exporter can read the baseline directly.

3. **Ring buffer statistics.** p95 of a 300-second rolling window, computed over co-collected
   samples. `quantile_over_time` exists in Prometheus but depends on scrape continuity, is
   computationally expensive at scale, and operates on scrape timestamps rather than collection
   timestamps. The ring buffer guarantees that all signals for a given poll timestep travel
   together regardless of scrape behaviour.

Scoring model changes do require binary redeployment. This is mitigated by keeping all thresholds
in the config file (key=value, hot-reloaded via inotify) — routine tuning never requires a binary
update, only structural scoring model changes do.

### Decision

Score at the edge, inline in the exporter poll thread on every cycle. Emit the score,
classification, and reason bitmask as Prometheus metrics. Emit all raw signals as metrics as
well. Operators act on the score; they debug against the raw signals.

### Consequences

- Score is always current (recomputed every poll cycle, ~1 Hz). No recording rule evaluation lag.
- Telemetry completeness gate is correct in all failure modes, including partial scrape failures.
- Baseline perf/W comparison is available without a separate pipeline.
- Scoring model structural changes require binary redeployment. Threshold tuning does not.
- The reason bitmask metric (`gpu_health_reasons`) surfaces why a score is low, so the score is
  not fully opaque even though the computation is in C rather than PromQL.
- dcgm-exporter (NVIDIA's maintained exporter) is not reused. This is an explicit choice: it
  emits raw field values and does not implement ring buffer statistics, completeness gating,
  baseline comparison, or scoring. The overlap in raw metrics is acceptable — operators may run
  both if they want dcgm-exporter's field coverage for other purposes.

---

## ADR-007: Fork-based privilege separation

**Status:** Accepted

### Context

The exporter has two distinct concerns with different privilege requirements:

- **Collection:** must call NVML/DCGM, which requires access to `/dev/nvidia*` and open file
  descriptors into the driver. Cannot be unprivileged.
- **Serving:** must accept TCP connections and emit HTTP responses. Should not have driver access.
  A vulnerability in the HTTP parser or metric renderer should not be able to touch GPU hardware
  or the filesystem.

Alternatives considered:

**Single process:** simple, but the HTTP surface inherits full driver privileges. Any memory
corruption in HTTP parsing can reach NVML handles.

**Two separate binaries communicating over a socket or pipe:** clean separation, but requires two
systemd units (or a supervisor), two binaries to deploy, and a filesystem-visible IPC resource.

**Fork before privilege drop:** one binary, one systemd unit. Parent retains driver access, forks
an HTTP child before dropping privileges, communicates via a socketpair created before fork. The
socketpair has no filesystem presence and is not inheritable by processes outside the lineage.

### Decision

Fork at startup. Parent: poll thread per GPU, NVML/DCGM access, drops all capabilities after GPU
handles are open. Child: HTTP server only, drops all capabilities and installs a seccomp-BPF
allowlist. IPC is a fixed-size `gpu_snapshot_t` struct written atomically over a socketpair —
no protocol parsing, no dynamic allocation in the IPC path.

Parent monitors child via `waitpid()` and respawns it on crash. Child detects parent death via
EOF on the socketpair and exits cleanly. Systemd monitors the parent only.

**Capability management — no libcap dependency.** Both parent and child drop capabilities via
the raw `capset(2)` syscall with `<linux/capability.h>` types. This is consistent with ADR-001
(zero mandatory non-system deps). libcap would add a runtime dependency for a single two-line
operation.

**Seccomp default action — fail-closed (`SECCOMP_RET_KILL_PROCESS`).** Any syscall not on the
allowlist kills the process immediately with no signal to the caller. The alternative,
`SECCOMP_RET_ERRNO(EPERM)`, would allow the child to continue after a policy violation and log
the error — useful for tuning but unacceptable for production. If the allowlist is missing a
syscall the child legitimately needs, the process crashes and the parent respawns it; the crash
is visible in logs and the missing syscall is diagnosable with `dmesg`. Fail-closed is the
correct default for a network-facing process on production infrastructure.

### Consequences

- HTTP child crash does not interrupt GPU collection. Parent respawns the child and scrapes
  resume after the respawn delay.
- A vulnerability in the HTTP parser cannot reach NVML handles or `/dev/nvidia*`. The child has
  no access to those resources after the capability drop and seccomp filter installation.
- Both parent and child drop all capabilities. For the parent, this means if NVML needs to
  re-open `/dev/nvidia*` after a driver reset (e.g. during error recovery), it will fail
  without `CAP_DAC_OVERRIDE`. That failure surfaces through the existing consecutive-error
  handling path and is an acceptable trade-off: driver resets are rare, the error is visible,
  and retaining capabilities to handle them would undermine the security model.
- ~110 lines of additional code over a single-process design. Justified for production deployment
  at AI factory scale.

---

## ADR-008: Socketpair IPC with fixed-size structs

**Status:** Accepted

### Context

The parent poll threads need to deliver current GPU state to the HTTP child for metric rendering.
IPC mechanism candidates: shared memory, named pipe, Unix domain socket with a protocol, or a
socketpair with fixed-size structs.

Shared memory: fast, but requires synchronisation primitives visible across the fork boundary,
and a file-backed segment has filesystem presence. POSIX shared memory (`shm_open`) requires
unlinking on cleanup.

Named pipe / Unix domain socket with framing protocol: requires protocol parsing in the HTTP
child, which is a complexity and security surface we are trying to eliminate.

Socketpair: created before fork, no filesystem presence, inheritable across fork, full-duplex.
Write of a fixed-size struct is atomic up to PIPE_BUF (4096 bytes on Linux). `gpu_ipc_msg_t`
(gpu_index + snapshot) fits within PIPE_BUF.

### Decision

Socketpair created before fork. IPC messages are fixed-size `gpu_ipc_msg_t` structs written with
a single `write()` call. The HTTP child drains the socketpair in a dedicated receiver thread and
maintains a snapshot array (one slot per GPU). The HTTP server thread reads from the snapshot
array. No protocol framing, no dynamic allocation in the IPC path.

### Consequences

- Write and read of `gpu_ipc_msg_t` are atomic — no partial reads, no framing needed.
- The HTTP child never allocates memory in the IPC hot path.
- If the parent writes faster than the child reads, the socketpair buffer fills and `write()`
  blocks. This is the correct back-pressure behaviour — it slows the poll thread rather than
  dropping data.
- Snapshot array in the child always holds the last known good state per GPU. HTTP requests for
  `/metrics` are served from this array, not by synchronously querying the parent. Slow clients
  do not stall the poll loop.

---

## ADR-009: Optional TLS via mbedTLS, not OpenSSL

**Status:** Accepted

### Context

The HTTP child serves on a TCP port. In a zero-trust or multi-tenant environment, scraping over
plain HTTP is not acceptable. TLS is needed.

OpenSSL: ubiquitous, well-understood, but carries significant CVE history, a large API surface,
and dynamic library linking that introduces version coupling. OpenSSL's API is also notably
complex for a simple server-side TLS wrapper.

Rustls (via C bindings): clean API, memory-safe implementation, but introduces a Rust toolchain
dependency at build time.

mbedTLS: BSD-licensed, lightweight (~300 KB), clean C API designed for embedding. No external
dependencies. Can be statically linked without OpenSSL CVE exposure. Covers the required use
case (server-side TLS, cert + key from config file) without exposing the full OpenSSL API
surface.

### Decision

TLS support is a compile-time option (`WITH_TLS=1`). When enabled, the HTTP child links mbedTLS
statically and wraps accepted file descriptors before request dispatch. When disabled, plain HTTP.
Cert and key paths are in the config file — if absent at runtime, startup fails with a clear
error rather than silently falling back to plain HTTP.

This is a Phase 1 TODO — the build flag and config keys exist; the `http.c` implementation is
pending.

### Consequences

- The binary with TLS is self-contained. No OpenSSL CVE surface.
- mbedTLS adds ~300 KB to the binary when enabled. Acceptable.
- The management network on target deployments is trusted; plain HTTP is operationally acceptable
  in the baseline case. TLS is implemented for environments where it is required, not assumed
  necessary everywhere.

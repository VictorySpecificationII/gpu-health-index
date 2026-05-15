---
name: Next session priorities
description: What to tackle at the start of the next session
type: project
---

Validate deployment in both TLS and non-TLS modes before moving to Phase 2.

**Why:** All bare metal validation so far has been WITH_TLS=1 on the H200. The plain HTTP path (no TLS build) has not been validated end-to-end — Prometheus scrape config, exporter startup, and full metrics flow all need a clean run without TLS to confirm the non-TLS path is production-ready.

**How to apply:** Spin up a fresh node, deploy with plain `make` (no WITH_TLS=1), confirm Prometheus scrapes over HTTP, confirm Grafana shows data, confirm alerts evaluate. Then repeat with WITH_TLS=1 as a second clean deployment.

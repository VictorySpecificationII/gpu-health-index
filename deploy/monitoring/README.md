# GPU Health Monitoring Stack

Prometheus + Grafana on Docker Compose. Runs on the GPU node itself.

## Prerequisites

- Docker + Docker Compose
- `gpu-health-exporter` running on port 9108

## Start

```sh
cd deploy/monitoring
docker compose up -d
```

Grafana: http://localhost:3000 (no login required)
Prometheus: http://localhost:9090

## Stop

```sh
docker compose down
```

## Notes

- Prometheus scrapes localhost:9108 every 5s
- Data retained for 7 days
- Dashboard auto-provisions on start — no manual import needed
- GPU serial number variable in the top bar filters all panels

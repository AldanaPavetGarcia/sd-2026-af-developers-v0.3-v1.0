# A&F Developers

Repository for the **Exodus Systems Inc. – Distributed Systems Engineering Program**.

This repository is managed under the **Exodus Engineering Workflow** used across all development teams participating in the course.

---

# Project Context

This repository is part of the **Exodus Systems Inc. Training Program** for the course:

**Sistemas Distribuidos – Ingeniería en Computación**

Students work as software engineers within a simulated company environment where they collaborate through modern development workflows, CI/CD pipelines and structured repository governance.

---

# Team Members

| Name              | GitHub               |
| ----------------- | -------------------- |
| Pavet Garcia, Aldana Micaela | @AldanaPavet |
| Soria, Federico Isaia | @BlastNeos |
|  | @ |

> The third member may be empty depending on team composition.

---

# Repository Structure

```

.
├── docs/               # Technical documentation, protocol specs; ADRs in docs/adr/
├── src/                # EOP server source code (v0.1)
├── hpc-engine/          # Parallel HPC graph-analysis engine (v0.2, C++/OpenMP)
├── services/go/         # Go services: API gateway (v0.3), see services/go/README.md
├── include/             # Shared public headers
├── lib/                 # Shared libraries (Common, Communications, WDog, Logger)
├── k8s/                 # Kubernetes manifests (Deployments, Jobs, Services, ConfigMaps)
├── deploy/production/   # EOP v1.0 product: the complete stack as one Kustomize overlay (ADR-029)
├── config/              # production.env: every production setting of the overlay (US-406 AC3)
├── tests/               # Test suite (unit, integration, smoke, k8s validation scripts)
├── monitoring/          # Observability stack config (SigNoz/OTel)
├── .github/            # CI/CD workflows and templates
├── README.md
└── LICENSE

```

See `docs/README.md` for the full architecture and per-Epic service breakdown, and
`docs/kubernetes-deployment-and-pod-recovery-guide.md` for build/deploy/validation steps.

Each team is free to extend the project structure depending on the needs of the implementation.

---

# Deploy EOP v1.0

The product is one Kustomize overlay, [`deploy/production`](deploy/production/kustomization.yaml)
(ADR-029): one command deploys the complete stack — eop-server (2 replicas with the hpc-engine
sidecar and the node registry in Redis), Redis, the API gateway (canary layout), the external
service stubs, Mosquitto and the edge ingestor, mttr-watcher, Prometheus (35-day retention on a
PersistentVolumeClaim), Grafana (every dashboard and alert provisioned), Loki and its agent, the
OTel collector and SigNoz. Dependencies converge inside the cluster (init containers, retries,
readiness probes, an idempotent schema Job): there is no step after the apply.

**Prerequisites** (once per cluster): an nginx Ingress controller and a default StorageClass. On
minikube: `minikube start --cpus 6 --memory 10g --disk-size 40g` and `minikube addons enable
ingress` (`standard` is the default StorageClass).

**Configuration:** every production value — ports, retention periods, SLO and alert thresholds,
registry and edge tuning — is in [`config/production.env`](config/production.env). Edit it and
apply again; `make production-config-test` checks it against the manifests and alert rules.

```bash
# 1. Images (built locally and loaded into the cluster; a registry release pins them in
#    deploy/production/kustomization.yaml, "images:")
docker build -t eop-server:local .
docker build -f hpc-engine/Dockerfile -t hpc-engine:local .
make gateway-image                     # api-gateway, external-stub, mttr-watcher, edge-ingestor, edge-sim
for i in eop-server hpc-engine api-gateway external-stub mttr-watcher edge-ingestor edge-sim; do
  minikube image load $i:local
done

# 2. Deploy and wait until everything is Ready
make k8s-prod-apply                    # kubectl apply -k deploy/production + readiness wait

# 3. Check it end to end: server, node registration, graph analysis, edge ingestion, gateway health
make smoke-test-prod
```

`make k8s-ready-time` measures US-406 AC4 (every workload Ready in under 3 minutes after the apply)
and must run on a cluster without EOP, for example a second minikube profile (`minikube start -p
eop-clean`, then load the images with `minikube -p eop-clean image load`). Never run it, nor
`kubectl delete -k deploy/production`, on a cluster whose data must be kept (the 30-day SLO run).

Access: `kubectl port-forward svc/grafana 3000:3000` (dashboards in the **EOP** folder),
`kubectl port-forward svc/api-gateway 8080:80` (`/v1/...`), `kubectl port-forward svc/signoz
3301:8080` (traces, http://localhost:3301). Edge devices connect to the broker's TLS listener on the node's port 30883;
their CA is `kubectl exec mosquitto-0 -- cat /mosquitto/certs/ca.crt` (or provide your own
certificates as the `mosquitto-tls` Secret before the apply). Operation — scaling, rollback,
reading the SLO dashboard, failover — is in [`docs/RUNBOOK.md`](docs/RUNBOOK.md).

The development path (`make k8s-apply`, the 7 GB-laptop profile) keeps applying `k8s/` directly.

---

# Architecture Decision Records

All decisions from v0.1 to v1.0 are in [`docs/adr/`](docs/adr/README.md) (numbering of the PRD
reference index, ADR-001 to ADR-014; the squad's own decisions from ADR-015):

| ADR | Decision |
|---|---|
| [ADR-001](docs/adr/ADR-001-ipc-mechanism.md) | Interprocess Communication (IPC) Mechanism |
| [ADR-002](docs/adr/ADR-002-server-concurrency-model.md) | Server Concurrency Model — Thread Pool |
| [ADR-003](docs/adr/ADR-003-message-protocol-design.md) | Messaging Protocol Design |
| [ADR-004](docs/adr/ADR-004-service-boundaries.md) | Service Limits and Responsibilities (EOP v0.1) |
| [ADR-005](docs/adr/ADR-005-observability-strategy.md) | Distributed Observability and Traceability Strategy |
| [ADR-006](docs/adr/ADR-006-containerization-and-kubernetes.md) | Containerization Strategy and Kubernetes Adoption |
| [ADR-007](docs/adr/ADR-007-parallelization-strategy-openmp.md) | Parallelization Strategy — OpenMP for Graph Algorithms |
| [ADR-008](docs/adr/ADR-008-hpc-engine-integration-pattern.md) | HPC Engine Service Boundary & Integration Pattern |
| [ADR-009](docs/adr/ADR-009-slo-definition-and-enforcement.md) | Observability, SLO Definition, and Dashboard Enforcement |
| [ADR-010](docs/adr/ADR-010-api-gateway-go.md) | API Gateway — Go, REST Contract and Async Job Store |
| [ADR-011](docs/adr/ADR-011-deployment-strategy-canary-blue-green.md) | Deployment Strategy — Canary and Blue-Green for the API Gateway |
| [ADR-012](docs/adr/ADR-012-circuit-breaker-configuration.md) | Circuit Breaker Configuration for External Services |
| [ADR-013](docs/adr/ADR-013-edge-protocol-mqtt.md) | Edge Protocol Selection — MQTT over gRPC (v1.0 design, PR #77) |
| [ADR-014](docs/adr/ADR-014-zephyr-thread-architecture.md) | ZephyrOS Thread Architecture (v1.0 design, PR #77) |
| [ADR-015](docs/adr/ADR-015-protocol-extension-and-trace-propagation.md) | Message Protocol Extension & Distributed Trace Propagation |
| [ADR-016](docs/adr/ADR-016-graph-representation.md) | Graph Representation Data Structure |
| [ADR-017](docs/adr/ADR-017-kubernetes-workload-management.md) | Kubernetes Workload Management and Deployment Strategy |
| [ADR-018](docs/adr/ADR-018-node-id-assignment.md) | Node_id Assignment Strategy and Reconnection Management |
| [ADR-019](docs/adr/ADR-019-structured-json-logs.md) | Persistence of Logs in Structured Format (JSON) |
| [ADR-020](docs/adr/ADR-020-ci-test-scaling.md) | Load and Stability Testing Adjustments for CI |
| [ADR-021](docs/adr/ADR-021-unified-signoz-stack.md) | Unified Local SigNoz Stack — Observability Runbook |
| [ADR-022](docs/adr/ADR-022-hpc-engine-node-id-convention.md) | HPC Engine Node_id Reservation Convention |
| [ADR-023](docs/adr/ADR-023-hpc-speedup-baseline.md) | HPC Speedup Baseline Strategy |
| [ADR-024](docs/adr/ADR-024-approximate-betweenness-centrality.md) | Approximate Betweenness Centrality for Large Graphs |
| ADR-025 | Pod-Crash MTTR (in the course repository as `ADR020-Pod-Crash-MTTR.md`; superseded in practice by ADR-027) |
| [ADR-026](docs/adr/ADR-026-centralized-logging-loki.md) | Centralized Logging — Loki, OTel Collector Agent and Trace Correlation |
| [ADR-027](docs/adr/ADR-027-shared-registry-and-mttr.md) | Shared Node Registry and Pod-Crash MTTR by Replica Failover (v1.0 design, PR #77) |
| [ADR-028](docs/adr/ADR-028-edge-ingestion.md) | Edge Ingestion — Delivery, Deduplication and Tracing (v1.0 design, PR #77) |
| [ADR-029](docs/adr/ADR-029-product-delivery-kustomize.md) | Product Delivery — One Kustomize Overlay and One Configuration File (v1.0 design, PR #77) |

---

# Development Workflow

This repository follows a **trunk-based collaboration model with protected branches**.

```

feature/*  →  development  →  main

```

### Branch Roles

| Branch        | Purpose                             |
| ------------- | ----------------------------------- |
| `main`        | Stable release branch               |
| `development` | Integration branch for new features |
| `feature/*`   | Individual work branches            |

---

# Contribution Rules

1. **Never commit directly to `main`**
2. All work must happen in a **feature branch**
3. All merges must go through **Pull Requests**
4. CI checks must pass before merging

Example workflow:

```

git checkout development
git checkout -b feature/my-feature

# work

git commit
git push

# open PR → development

```

---

# Pull Requests

Every Pull Request must:

- Explain the purpose of the change
- Reference the related issue
- Pass CI checks
- Receive at least **one approval**

The repository enforces these rules automatically via **GitHub Rulesets**.

---

# Continuous Integration

All Pull Requests trigger automated checks:

- build
- test

The PR cannot be merged unless these checks succeed.

---

# Issue Management

Work is organized through **GitHub Issues**.

Typical issue types include:

- `type: feature`
- `type: bug`
- `type: docs`
- `type: chore`

Priority labels:

- `priority: high`
- `priority: medium`
- `priority: low`

---

# Engineering Principles

Teams are expected to follow standard engineering practices:

- small incremental commits
- readable commit messages
- testable code
- code reviews before merging
- documentation of architectural decisions

---

# Development Environment

Each team is responsible for defining its own local development environment depending on the technologies chosen for the implementation.

However, all repositories must maintain:

- reproducible builds
- automated tests
- CI compatibility

---

# Observability

Prometheus and Grafana ship with the stack (#43), scraping `eop-server` and `hpc-engine`'s
`/metrics` endpoints and auto-provisioning the SLO and HPC dashboards — no manual import needed.

**docker-compose:**

```bash
docker compose up -d prometheus grafana eop-server
```

- Prometheus targets: `http://localhost:9090/targets` — `eop-server` should show as `UP`.
- Grafana: `http://localhost:3000`, login `admin` / `admin` (default credentials, not changed
  for this dev/demo stack) → **Dashboards** (left sidebar) → folder **EOP**.

**Kubernetes:**

```bash
make k8s-apply
kubectl port-forward svc/prometheus 9090:9090 &
kubectl port-forward svc/grafana 3000:3000 &
```

Same URLs and login as above once the port-forwards are up. `eop-server` and `hpc-engine` are
both discovered dynamically via pod annotations (`prometheus.io/scrape|path|port`) — same
mechanism for both workloads, no static targets to keep in sync.

Two dashboards are provisioned:

- **EOP SLO Dashboard** — system-level health (availability, error budget, request latency).
  Panel-by-panel reference: [`docs/slo-dashboard-grafana.md`](docs/slo-dashboard-grafana.md).
- **EOP HPC Performance Dashboard** — `hpc-engine` graph-processing performance. Panel-by-panel
  reference: [`docs/hpc-performance-grafana-panel.md`](docs/hpc-performance-grafana-panel.md).

**No data in the panels?** Send some real traffic first — the dashboards only show what
Prometheus has actually scraped, they don't backfill history. See the linked docs above for a
minimal example per service and what a normal "empty panel" (vs. a broken one) looks like.

---

# License

This repository is part of an **academic engineering program** and is intended exclusively for educational purposes.

---

# Exodus Systems Inc.

> _Engineering the distributed systems of tomorrow._

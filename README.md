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
├── docs/               # Technical documentation, ADRs, protocol specs
├── src/                # EOP server source code (v0.1)
├── hpc-engine/          # Parallel HPC graph-analysis engine (v0.2, C++/OpenMP)
├── include/             # Shared public headers
├── lib/                 # Shared libraries (Common, Communications, WDog, Logger)
├── k8s/                 # Kubernetes manifests (Deployments, Jobs, Services, ConfigMaps)
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

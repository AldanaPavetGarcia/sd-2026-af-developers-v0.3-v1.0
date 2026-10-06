# Architecture Decision Records

ADRs follow the numbering of the PRD reference index (ADR-001 to ADR-014). Decisions the squad
added beyond that index are numbered from ADR-015 onwards, in the order they were taken.

## PRD reference index

| ADR | Title | Milestone | Status |
|---|---|---|---|
| [ADR-001](ADR-001-ipc-mechanism.md) | IPC Mechanism — BSD Sockets over RPC Frameworks | v0.1 | Accepted |
| [ADR-002](ADR-002-server-concurrency-model.md) | Server Concurrency Model | v0.1 | Accepted |
| [ADR-003](ADR-003-message-protocol-design.md) | Message Protocol Design | v0.1 | Accepted |
| [ADR-004](ADR-004-service-boundaries.md) | Service Boundaries | v0.1 | Accepted |
| [ADR-005](ADR-005-observability-strategy.md) | Observability Strategy | v0.1 | Accepted |
| [ADR-006](ADR-006-containerization-and-kubernetes.md) | Containerization and Kubernetes Adoption | v0.1 | Accepted |
| [ADR-007](ADR-007-parallelization-strategy-openmp.md) | Parallelization Strategy — OpenMP | v0.2 | Accepted |
| [ADR-008](ADR-008-hpc-engine-integration-pattern.md) | HPC Engine Integration Pattern | v0.2 | Accepted |
| [ADR-009](ADR-009-slo-definition-and-enforcement.md) | SLO Definition and Enforcement | v0.2 | Accepted |
| ADR-010 | API Gateway Language — Go | v0.3 | Pending (#50) |
| ADR-011 | Deployment Strategy — Canary and Blue-Green | v0.3 | Pending (#50) |
| ADR-012 | Circuit Breaker Configuration | v0.3 | Pending (#50) |
| ADR-013 | Edge Protocol Selection — MQTT over gRPC | v1.0 | Reserved |
| ADR-014 | ZephyrOS Thread Architecture | v1.0 | Reserved |

## Squad decisions beyond the PRD index

| ADR | Title | Milestone |
|---|---|---|
| [ADR-015](ADR-015-protocol-extension-and-trace-propagation.md) | Message Protocol Extension & Distributed Trace Propagation | v0.2 |
| [ADR-016](ADR-016-graph-representation.md) | Graph Representation Data Structure | v0.2 |
| [ADR-017](ADR-017-kubernetes-workload-management.md) | Kubernetes Workload Management and Deployment Strategy | v0.2 |
| [ADR-018](ADR-018-node-id-assignment.md) | Node_id Assignment Strategy and Reconnection Management | v0.1 |
| [ADR-019](ADR-019-structured-json-logs.md) | Persistence of Logs in Structured Format (JSON) | v0.1 |
| [ADR-020](ADR-020-ci-test-scaling.md) | Load and Stability Testing Adjustments for CI | v0.1 |
| [ADR-021](ADR-021-unified-signoz-stack.md) | Unified Local SigNoz Stack — Observability Runbook | v0.1 |
| [ADR-022](ADR-022-hpc-engine-node-id-convention.md) | HPC Engine Node_id Reservation Convention | v0.2 |
| [ADR-023](ADR-023-hpc-speedup-baseline.md) | HPC Speedup Baseline Strategy | v0.2 |
| [ADR-024](ADR-024-approximate-betweenness-centrality.md) | Approximate Betweenness Centrality for Large Graphs | v0.2 |
| ADR-025 | Pod-Crash MTTR | v0.2 |
| ADR-026 | Centralized Logging — Loki | v0.3 |

ADR-025 is open in the course repository as `ADR020-Pod-Crash-MTTR.md` (PR #68) and is renumbered
here when it is synced. ADR-026 is written in #50.

## Previous numbering

Until v0.2 the ADRs lived in `docs/` with the squad's own sequential numbering. References in code,
tests, configuration and the CHANGELOG were updated to the numbers below.

| Previous | Current |
|---|---|
| ADR001–ADR006 | ADR-001–ADR-006 (unchanged) |
| ADR007 | ADR-008 |
| ADR008 | ADR-015 |
| ADR009 | ADR-016 |
| ADR010 | ADR-007 |
| ADR011 | ADR-017 |
| ADR012 | ADR-009 |
| ADR013 | ADR-018 |
| ADR014 | ADR-019 |
| ADR015 | ADR-020 |
| ADR016 | ADR-021 |
| ADR017 | ADR-022 |
| ADR018 | ADR-023 |
| ADR019 | ADR-024 |
| ADR020 (course repository) | ADR-025 |

# Changelog

All notable changes to the EOP Server Node Registry project will be documented in this file.

## [Unreleased] - 2026-08-08

### Added
- **HPC Engine persistent daemon mode:** `hpc-engine` now supports a socket-connected mode — triggered by passing `--server-host`/`--server-port`/`--node-id` — that registers with the EOP server as a peer and keeps servicing `ANALYZE_GRAPH` requests indefinitely, with bounded connect/register retry-backoff and a heartbeat file for external liveness checks. Without those flags it falls back to its original one-shot batch mode (stdin/stdout), so both `hpc-engine` (#13/#14) tests and callers are unaffected.
- **HPC Engine Docker image:** dedicated multi-stage `hpc-engine/Dockerfile`; the runtime stage installs `libgomp` (OpenMP runtime) and has no `EXPOSE`, since the engine is an outbound-only TCP client — it never binds or listens.
- **Kubernetes Deployment for `hpc-engine`** (`k8s/hpc-engine-deployment.yaml`): persistent Deployment with a liveness probe (process-alive check), a readiness probe (heartbeat-file freshness), CPU/memory requests and limits, and the shared `eop-global-config` ConfigMap.
- **Kubernetes batch Job demo** (`k8s/jobs/hpc-engine-batch-job.yaml` + sample-graph ConfigMap): one-shot `ANALYZE_GRAPH` run via a `Job`, exercising the batch execution path alongside the persistent Deployment — the Dual-Execution Vector described in `docs/ADR011-Kubernetes-workload-management.md`.
- **CI:** the Docker image build matrix now covers both `eop-server` and `hpc-engine`, each validated against the 50MB runtime image size limit; a new `k8s-manifest-lint` job runs `kubeconform -strict` over every manifest under `k8s/`, including Jobs.
- **K8s validation scripts:** `tests/k8s/hpc-engine-recovery-cycle.sh` and `tests/k8s/rollout-restart-under-load.sh`.
- **Docs:** `docs/kubernetes-deployment-and-pod-recovery-guide.md` — build/deploy/validate guide covering both `eop-server` and `hpc-engine` workloads (probes, restart, reconnect/re-registration behavior).
- **Merged Welcome Message (ACK):** The server now responds to a successful `REGISTER` command with a unified greeting/ACK string formatted as `ACK Server v1.0 | TIME <timestamp> | SESSION_ID <node_id>`.

### Changed
- **Shared K8s ConfigMap renamed:** `eop-server-config` → `eop-global-config`, now also carrying `hpc-engine`'s configuration (`HPC_NODE_ID`, `HPC_BUNKER_NAME`, `HPC_CAPACITY`, `OMP_NUM_THREADS`, `EOP_SERVER_HOST`).
- **Direct Node Registration:** Replaced the legacy two-step session process. The initialization of a session directly relies on parsing a `REGISTER` command from an accepted connection.
- **Node ID Decoupling:** Re-adapted the connection limit verification. Allowed connection count (`MAX_CLIENTS=10`) is exclusively checked against the global active connections tracker instead of bounding numerical ranges from `node_id`.
- **Integration Test Overhaul:** Updated the `testConnection.cpp` `Reaches100ConnectionsSuccessfully` test case to perform the 100-cycle loop (Connect → Register → Acknowledge → Disconnect) utilizing the standard unified protocol, dropping its dependency on deprecated session allocations.

### Removed
- **Legacy REQ_SESSION System:** Extracted and eliminated the legacy `isLegacySession` logic, `REQ_SESSION` command parsing, and numeric `activeSessions` indexing structures.

### Fixed
- **Static Analysis & Linting Warnings (`clang-tidy`):**
  - Included a strict prototype `(void)` definition on `watchdog_get_current_time_ms` in `timed_watchdog_controller.c` to fix warnings natively triggered under standard C flags.
  - Added socket connection bounds checking (`ASSERT_GE(sock, 0)`) in test files to resolve unvalidated `-1` descriptor assertions flagged by UNIX library analyzers.
- **Code Styling Constraints:** Formatted the entire core codebase (`.cpp`, `.hpp`, `.h`) automatically conforming flawlessly to `clang-format-18` style guides.
- **EOF Protocol Warnings:** Fixed ending empty-line parsing problems inside `commonProtocol.hpp`.

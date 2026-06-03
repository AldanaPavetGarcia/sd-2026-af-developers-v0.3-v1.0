# Changelog

All notable changes to the EOP Server Node Registry project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-06-03

### Added
- **Core Server & Connection Handshake:** Implemented the sockets communication engine, environment-based configuration, and connection session handshake.
- **Modular Architecture:** Structured the EOP server code using specialized libraries for config loading, protocol parsing, and event auditing.
- **Concurrency Model (Thread Pool):** Refactored the server's thread pool to a message-based asynchronous queue processing model to handle concurrent client registrations.
- **Graceful Shutdown (SIGTERM):** Implemented signal handler for SIGTERM to gracefully stop accepting connections, wait for active sessions to finish within a timeout, and shut down OTel tracing.
- **Client SDK (`libeop`):** Developed the client library with a public API via opaque handlers, thread-safety, non-blocking connection logic, memory leak safety, and a `connect_basic` example.
- **Distributed Tracing (OpenTelemetry):** Integrated `opentelemetry-cpp` to trace core server operations (`REGISTER`, `QUERY_NODE`, `LIST_NODES`) and propagate trace contexts.
- **Observability Stack (SigNoz):** Configured a unified `signoz-standalone` stack with ClickHouse, OTel Collector, and SigNoz UI dashboard in `docker-compose.yml`.
- **Structured JSON Logging:** Upgraded server logging to output JSON records correlated with OpenTelemetry trace contexts.
- **Dockerization:** Configured a multi-stage Docker build pipeline utilizing `vcpkg` manifest mode caching to generate minimal runtime container images.
- **Kubernetes Deployment:** Added Kubernetes manifests (`deployment.yaml`, `service.yaml`, `configmap.yaml`) supporting liveness/readiness TCP probes, pod restart policies, and environment config.
- **Kubernetes Pod Recovery Test:** Implemented automated validation scripts (`pod-recovery-cycle.sh` / `deploy-local-smoke.sh`) verifying service restoration after pod failures.
- **Architecture Decision Records (ADRs):** Documented architecture decisions formally in ADR001 through ADR010.
- **Welcome Message (ACK):** Added greeting/ACK string response (`ACK Server v1.0 | TIME <timestamp> | SESSION_ID <node_id>`) upon successful node registration.

### Changed
- **Server Event Loop Optimization:** Refactored the core connection loop to utilize `poll()` multiplexing and reduced heartbeat logs verbosity.
- **Direct Node Registration:** Replaced the legacy two-step session process. The initialization of a session directly relies on parsing a `REGISTER` command.
- **Node ID Decoupling:** Re-adapted connection limit verification (`MAX_CLIENTS=10`) against a global active connections tracker instead of bounding numerical ranges from `node_id`.
- **Test Scaling Adjustments:** Scaled down stress testing constraints in latency benchmarks and connection tests to optimize execution speed within CI pipelines.
- **Documentation Restructuring:** Reorganized system architecture guides, runbooks, and benchmarking instructions in `README.md`.

### Removed
- **Legacy REQ_SESSION System:** Extracted and eliminated the legacy `isLegacySession` logic, `REQ_SESSION` command parsing, and numeric `activeSessions` indexing structures.

### Fixed
- **ThreadSanitizer Data Races:** Resolved data races in time-formatting functions, the `isTerminationSignalReceived` flag, and concurrent unit test assertions.
- **OTel-to-SigNoz Integration Pipelines:** Resolved ClickHouse schema initialization mismatch and connectivity problems in smoke integration tests.
- **Client SDK Connection & Formatting:** Updated default connection ports and formatted client codebase to ensure consistent brace styling and compliance with project style guides.
- **Static Analysis & Linting Warnings (`clang-tidy`):** Fixed warnings in `timed_watchdog_controller.c` and UNIX library socket assertions.


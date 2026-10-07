# ADR-006: Containerization Strategy and Kubernetes Adoption

## Context and Problem

The EOP system must be delivered to Vault-Tec as a packaged, executable product, not as isolated source code. The client operates a Kubernetes cluster and requires the infrastructure to be manageable by their operations team without requiring them to know the internal code implementation. Additionally, the system development requires engineers to master cloud-native workloads, process lifecycle management (POSIX signals), health probes, and continuous deployment strategies from the initial **v0.1** phase.

## Options Considered

1. **Virtual Machine (VM)-Based Deployment:** Offers complete isolation but introduces high resource overhead, slow iteration cycles, and is not aligned with modern distributed systems practices.

2. **Exclusive Use of Docker Compose (No Kubernetes):** Excellent for local development, but lacks essential primitives for advanced project phases, such as automated pod lifecycle management, health-based restarts, or Canary deployment strategies.

3. **Docker Containers with Kubernetes Orchestration since v0.1:** Standardizes Kubernetes as the operating and final target environment from day one, mitigating the accumulation of technical debt in the infrastructure.

## Decision

It has been decided to adopt **Mandatory Containerization with Docker and Deployment on Kubernetes (Option 3) starting with version v0.1**. To optimize the engineers' workflow, the entire local development stack will be consolidated and orchestrated using **Docker Compose**.

## Justification (Trade-off Analysis)

* **Controlled Shutdown and Signal Management:** By enforcing the use of the JSON array format in the `ENTRYPOINT`, it is ensured that the C++ binary executes with **PID 1**, allowing the direct propagation of the `SIGTERM` signal for an orderly shutdown (maximum 15 seconds) without abrupt interruptions for active clients.

* **Efficiency in Runtime Environments:** The restriction to using *Multi-stage builds* separates the compilation toolchain from the runtime environment, resulting in ultra-lightweight images (e.g., 50 MB) based on optimized distributions (e.g., `debian:bookworm-slim`, `alpine`, or `distroless`).

* **Active Operational Resilience:** Kubernetes manifests will delegate fault monitoring to automated liveness and readiness probes, guaranteeing the restart of downed pods in less than 10 seconds (US-109).

* **Local Development Agility:** Adopting Docker Compose as the local development environment unifies the startup of the EOP server, OTel Collector, and SigNoz into a single command (`docker compose up`) in less than 60 seconds, accelerating the team's integration testing.

## Consequences

* **Positive:** Identical and reproducible deployments on any infrastructure; automated manifest validation in continuous integration (CI) using `kubeval`/`kubeconform`; clean configuration injection using `ConfigMaps` without hardcoded values ​​in the images.

* **Drawbacks:** Steep initial learning curve for engineers accustomed only to traditional local environments.

* **Technical Restrictions and Invariants:**

* **Resource Contract:** All Kubernetes manifests must explicitly declare the `resources.requests` and `resources.limits` sections for both CPU and memory.

* **Network Policies:** The server must listen on the TCP port injected by the `EOP_PORT` environment variable (default 9026) and explicitly fail if the port is busy (US-101).

* **Local Environment:** The local complementary cluster (`minikube` or `k3s`) and the CI image registry paths must be explicitly documented in the `README.md` file of the machine.

--

# ADR-017: Kubernetes Workload Management and Deployment Strategy

## Context and Problem

Moving EOP v0.2 into a cluster environment requires an automated, self-healing deployment architecture. We must transition from manual binary execution (`./eop_server`) toward an environment where process lifecycles, execution topologies, and resource quotas are declared as code and completely managed by the orchestrator. The solution must support both persistent availability and single-batch computation routines.


## Decision

The full EOP v0.1 and v0.2 stack will be fully containerized via multi-stage Dockerfiles and deployed natively using structured **Kubernetes Manifests**. The architecture implements an decoupled orchestration model:

```
[Kubernetes Cluster]
   ├── ConfigMap: eop-global-config (Ports, Timeouts, OMP_NUM_THREADS)
   ├── Deployment: eop-server (ReplicaSet: 1, Liveness/Readiness Sockets)
   └── Deployment/Job: hpc-engine (Horizontal pods or Single Batch Tasks)

```

### Key Architectural Requirements:

1. **Resource Constraints:** Every deployment artifact must enforce explicit `limits` and `requests` blocks for CPU and Memory to prevent resource starvation or unmapped cluster nodes.

2. **Dual-Execution Execution Vector:** The `hpc-engine` will be packaged to support two distinct operation modes:

* **Persistent Deployment Mode:** Continuous availability for processing real-time, ad-hoc graph payloads.

* **Batch Job Mode:** Spawning dedicated Kubernetes `Job` pods that spin up on demand to process massive long-running topologies, returning execution code `0` upon completion.


## Alternatives Considered

* **Bare Metal/Systemd Unit files:** Rejected. This approach lacks native health checking, prevents horizontal scaling of calculation components, and requires manual server configuration across different testing sites.

* **Docker Compose Only:** Highly effective for local testing and developer velocity, but lacks the advanced orchestration capabilities required for production environments, such as readiness probes, automated rollouts, and granular resource constraints.


## Consequences

* **Positive:** Centralized configuration. Modifying internal values like network ports, idle timeouts, or thread allocation sizes (`OMP_NUM_THREADS`) is handled by updating a central `ConfigMap` without rebuilding container images.

* **Drawbacks:** Significantly increases the volume of YAML configuration files that must be maintained and validated.

* **Restrictions Introduced:**

* All cluster manifests must pass automated linting checks (`kubeval` or `kubeconform`) within the CI pipeline.

* Rolling restarts must be compatible with the v0.2 workload requirements and validated by the corresponding US-204 integration test. The server and HPC engine must rely on readiness probes so traffic is only routed to Ready pods.

* The build system must expose a unified hook (`make k8s-apply`) that automatically handles the sequence of applying configurations and blocking until all components transition into a healthy `Ready` state.

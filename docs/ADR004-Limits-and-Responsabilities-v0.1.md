# ADR 004: Service Limits and Responsibilities (EOP v0.1)

## Context and Problem

The EOP (Exodus Orbital Protocol) system is being developed by multiple squads in parallel. To avoid overlapping responsibilities, ensure interoperability in the Proto-Negotiation Meeting, and guarantee a clean integration into Kubernetes (Lab 1), it is essential to define which components this service owns exclusively, what operations it exposes/consumes, what data it manages as an authoritative source, and what guarantees it offers to external clients.

## Options Considered

1. **Absolute Technical Monolith:** Group the network server, business logic, database, and visualization into a single binary. (Rejected: It violates the principles of distributed systems and deployment on Kubernetes).

2. **Fragmented Microservices in v0.1:** Separate socket management, node registration, and the telemetry subsystem into independent micro-containers from day one. (Rejected: Adds unnecessary network overhead and debugging complexity at an early stage.)

3. **Central Infrastructure Server with Decoupled Integration Library:** Design a centralized C++ server (responsible for network, in-memory state, and OTel export) complemented by a C client library (`.so` / `.a`) isolated using opaque types.

## Decision

It has been decided to implement **Option 3: Central Infrastructure Server with Decoupled Integration Library**. The service boundaries and contracts are defined under the following structural responsibility matrix.

## Service Boundaries Structure (Service Owned)

| Field | Technical Content |

| --- | --- |

| **Service Name** | `eop-server-svc` (Used uniformly in Kubernetes manifests, OTel attributes, and Grafana). |

**Responsibility** | Manage the lifecycle of concurrent TCP bunker connections, persist node state in memory, process protocol commands, and centralize the transmission of structured telemetry. |

**Operations Exposed** | JSON protocol messages: `REGISTER`, `QUERY_NODE`, `LIST_NODES`, and `HEARTBEAT`. |

**Operations Consumed** | Export of OTel Spans (native gRPC/Protobuf) to the external container `otel-collector`. |

**State Owned** | Authoritative memory source (`std::map`/`std::unordered_map` protected by mutexes) of `NodeRegistry`: mapping of `node_id` to metadata (`bunker_name`, `ip_address`, `capacity`, `ONLINE`/`OFFLINE` status) and `session.id`. |

**Guarantees Provided** | - Minimum support for 100 concurrent clients without latency degradation > 2x (US-102).<br>

<br>- Responses to `REGISTER` in < 200 ms (US-103).<br>

<br>- Disconnection detection (SIGKILL or FIN) in < 10 seconds, changing the status to `OFFLINE` (US-104).<br>

<br>- Sending Spans to SigNoz in < 30 seconds after the operation (US-110). |

## Explicit No-Responsibilities (Out of Scope)

To avoid scope creep in v0.1, this service **DOES NOT** perform the following tasks:

* **Disk Persistence:** All `NodeRegistry` state resides strictly in RAM (US-103 AC4). If the Pod is restarted due to failures or by a K8s action (US-109), the data is lost, and clients must reconnect and re-register their nodes independently.

* **Network Graph Analysis:** The `ANALYZE_GRAPH` operation is explicitly outside the scope of this milestone (reserved for the v0.2 protocol extension).

* **Metrics and Alerts Aggregation:** No scraping endpoints are exposed for Prometheus, and no dashboards are configured in Grafana (reserved for v0.2).

* **Log Centralization with Loki:** Log output is strictly limited to `stdout` in structured JSON format. Log collection and indexing by log agents are excluded (reserved for v0.3).

* **Cryptography or TLS Handling:** BSD socket connections are in plain text. Transport layer security is not managed by the server at this stage.

## Dependencies

The service has strict dependencies on the deployment environment infrastructure:

1. **OTel Collector (`otel-collector`):** It is assumed that the collector container is available on the local Docker Compose/K8s network to receive and process the distributed trace buffers before sending them to SigNoz.

2. **Kubernetes Kubelet:** It depends on the K8s scheduler to run the availability (*liveness*) and readiness (*readiness*) probes every 10 seconds, ensuring the Pod restarts in case of catastrophic failures in < 10 seconds (US-109).

## Consequences

* **Positive:** The contract for the protonodequacy meeting is perfectly defined, allowing the development of the client library in C (Epic E2) to progress in parallel and independently using the public API of `eop_client.h`.

* **Drawbacks:** The lack of disk persistence necessitates designing client logic to anticipate automatic re-registration scenarios after server failure recovery (US-109 AC6).

* **Technical Restrictions:** The service names established here are binding for OpenTelemetry semantic attributes. Any changes to the structure of exposed JSON messages will break compatibility and invalidate automated integration tests.
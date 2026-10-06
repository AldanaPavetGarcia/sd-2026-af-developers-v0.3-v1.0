# ADR-008: HPC Engine Service Boundary & Integration Pattern

## Context and Problem

The Exodus Ops Platform (EOP) v0.2 milestone introduces a high-performance computing (HPC) graph analysis capability. The platform must compute shortest paths (Dijkstra), connected components, and centrality analysis on massive bunker topology networks. The core architectural problem is determining where this heavy computational logic should reside: inside the existing v0.1 communication server, compiled locally inside the client, or isolated as an autonomous component.

Additionally, the integration pattern must define how this component communicates within the ecosystem. The system requirement states that the execution flow must follow a strict path:

$$\text{C Client} \longrightarrow \text{EOP v0.1 Server} \longrightarrow \text{HPC Engine} \longrightarrow \text{EOP v0.1 Server} \longrightarrow \text{C Client}$$

The client must interact exclusively with the v0.1 server, maintaining complete product and workflow continuity.


## Decision

It has been decided to isolate the graph analysis logic into a standalone C++ process named `hpc-engine`. This component will have no direct public interface for standard clients. Instead, it will integrate into the network by acting as an **EOP Node**, registering itself directly with the v0.1 server upon initialization via the existing socket-based `REGISTER` protocol.

```
+----------------+             +-------------------+             +----------------+
|    C Client    | ----------> |  EOP v0.1 Server  | ----------> |   HPC Engine   |
| (ANALYZE_GRAPH)| <---------- | (Request Routing) | <---------- |   (OpenMP)     |
+----------------+             +-------------------+             +----------------+
```

When an external client submits an `ANALYZE_GRAPH` transaction, the v0.1 server acts as an internal reverse-proxy, verifying engine availability, forwarding the payload over the active socket descriptor, enforcing a configured transaction timeout, and routing the results back to the source client.


## Alternatives Considered

| Alternative | Reason For Rejection |
| --- | --- |
| **Monolithic Server Integration** (Compile algorithms directly inside the v0.1 server binary) | Simplifies the deployment topology by maintaining a single binary, but tightly couples raw networking/session state with intensive CPU-bound processing. A single malformed graph computation could starve the main socket selector loop, dropping incoming bunker connections and violating availability constraints. |
| **Fat Client-Side Implementation** (Execute algorithms locally in the C library) | Bypasses server overhead, but forces client environments to possess heavy computational resources. It completely invalidates the platform-as-a-service model required by Vault-Tec and prevents centralized caching or orchestration. |
| **Standalone Daemon with independent API (e.g., gRPC)** | Creates an architectural fragmentation. It forces clients to manage connections to multiple endpoints and violates **ADR-001**, which explicitly mandates custom socket communication over third-party RPC frameworks for the core platform. |


## Justification (Trade-off Analysis)

* **Separation of Concerns:** The v0.1 server remains a highly optimized, communication server focused on connection lifecycles, session pools, and basic routing. The `hpc-engine` owns the CPU-bound, parallel algorithmic domain.

* **Reuse of Infrastructure:** By forcing the engine to connect via the `REGISTER` message, we reuse 100% of the socket framing, error handling, and connection state machines developed in v0.1.

* **Fault Isolation & Resilience:** If the `hpc-engine` component experiences an unhandled failure or crash, the main server intercepts the broken pipe and gracefully returns an `ERROR_SERVICE_UNAVAILABLE` payload to the client. The core communication platform never hangs or crashes due to external engine dropouts.


## Consequences

* **Positive:** Clear, independent maintainability paths; graph algorithms can be thoroughly unit-tested without mock socket servers; horizontal scaling of computing nodes is enabled natively.

* **Drawbacks:** Increased configuration and setup complexity. The v0.1 server dispatcher must be extended to track the registration status of specialized computing nodes and handle asynchronous packet proxying.

* **Restrictions Introduced:** The server must enforce strict non-blocking timeouts when waiting for the engine response. It is strictly forbidden for the server to block indefinitely on a registered engine descriptor.

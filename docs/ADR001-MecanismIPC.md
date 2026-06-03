# ADR 001: Interprocess Communication (IPC) Mechanism

## Context and Problem

The EOP (Exodus Orbital Protocol) platform requires an interprocess communication (IPC) system for data exchange between distributed services. The problem lies in choosing the appropriate level of abstraction: using modern frameworks (such as gRPC or ZeroMQ) simplifies development but obscures network management. For version **v0.1**, the central engineering objective is for students to work directly at the transport layer, managing file descriptors, message framing, and network concurrency.

## Options Considered

1. **RPC Frameworks (gRPC / Protobuf / ZeroMQ):** These offer high productivity and automatic serialization. However, they abstract the socket and transport layers, preventing the learning of the underlying network mechanisms (the main objective of v0.1).

2. **Messaging Brokers (RabbitMQ / Kafka):** Introduce a complex third-party infrastructure that shifts the problem domain to broker management rather than the network.

3. **Unix Domain Sockets:** Efficient for local communication, but limited to a single host. EOP, from its initial design, requires communication between nodes over IP networks.

4. **Pure BSD/POSIX Sockets:** Low-level communication using the standard sockets API (`socket()`, `bind()`, `send()`, `recv()`).

## Decision

It has been decided to use **Direct BSD/POSIX Sockets** (Option 4) for the implementation of both the server (C++) and the client (C).

## Justification (Trade-off Analysis)

* **Alignment with Learning Objectives:** This option forces engineers to confront real-world problems in distributed systems: handling partial reads, manual buffer management, and designing message framing protocols.

* **Full Control and Ownership of the Protocol:** By not depending on a framework, the team has absolute control over how data is serialized and how system resources are managed. The protocol design is an explicit decision documented in **ADR-003**, not automatically generated code.

* **Professional Competence:** Mastery of the POSIX sockets API is a fundamental skill transferable to the development of critical infrastructure (databases, game engines, and high-performance servers).

## Consequences

* **Positive:** Development of direct skills in file descriptor management; full control over the error and concurrency model; the system is free of heavy external dependencies.

* * **Drawbacks:** Significant increase in implementation effort (specifically in handling partial `recv()` returns). The protocol's evolution requires rigorous manual negotiation to maintain compatibility.

* **Restrictions Introduced:** The use of **RAII** for resource management (file descriptors) becomes mandatory according to **NFR-3**, and the concurrency model for handling multiple clients must be explicitly documented in **ADR-002**.
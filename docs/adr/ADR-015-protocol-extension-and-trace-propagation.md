# ADR-015: Message Protocol Extension & Distributed Trace Propagation

## Context and Problem
The introduction of high-performance computing (HPC) capabilities in **EOP v0.2** requires a new mechanism for analysts to submit topology graphs and receive critical analyses. The architectural problem is divided into two areas:

1. **Protocol Evolution:** How to extend the network contract defined in v0.1 without breaking compatibility with existing messages (`REGISTER`, `QUERY_NODE`, `LIST_NODES`, `HEARTBEAT`) while preserving the underlying socket infrastructure.
2. **Asynchronous Context Propagation:** Since the flow traverses in a distributed and inter-process manner ($\text{Client C} \rightarrow \text{Server v0.1} \rightarrow \text{HPC Engine}$), and considering that we operate over raw TCP sockets with JSON (without HTTP or gRPC), OpenTelemetry’s automated tools cannot inject or extract tracing metadata on their own. We need an explicit standard so that SigNoz *spans* are linked under the same `trace_id`.

## Decision
The following architectural design decisions have been made:

1. **Non-Breaking Extension (Additive Extension):** Introduce a new operation token, `ANALYZE_GRAPH`, into the protocol’s JSON envelope, strictly adhering to the `\n` delimiter framing defined in **ADR-003**. The detailed contract for fields, error schemas, and validations is delegated to the team’s reference technical document (`docs/analyze-graph-protocol-v0.2.md`).
2. **Adoption of W3C Trace Context over Raw TCP:** Instead of designing a proprietary telemetry format, the decision is to adopt the industry-standard **W3C Trace Context** and inject it directly as top-level fields within the protocol’s JSON payload using the keys `traceparent` and `tracestate`.

```
[Client C (Generates Context)]
    │
    ▼ (JSON with 'traceparent' + \n)
[Server v0.1 (Creates Routing Span and injects context)]
    │
    ▼ (Forwarded JSON + \n)
[HPC Engine (Extracts 'traceparent' / Launches OpenMP threads)] ──> Unified visualization in SigNoz
```

## Alternatives Considered

* **Create a Separate Port and Binary Protocol for HPC:** Opening a second socket dedicated exclusively to the HPC engine with binary serialization was evaluated to optimize performance. It was rejected because it fragmented the system architecture, forced C clients to manage multiple connections, and violated the principle that Server v0.1 should centralize workflow execution (Workflow Continuity).
* **Out-of-Band Telemetry Propagation (Through Files or an External Database):** Storing trace identifiers in shared storage for retrieval by the Server and Engine was considered. It was rejected because it introduces I/O latency, adds storage dependencies, and creates race conditions that undermine the real-time observability required by the SLO dashboard.

## Justification (Trade-off Analysis)
* **Backward Compatibility:** Because the extension is additive, existing v0.1 clients that continue using `REGISTER`, `QUERY_NODE`, `LIST_NODES`, and `HEARTBEAT` remain unaffected. Servers that do not implement `ANALYZE_GRAPH` should reject it as an unsupported operation rather than reinterpret or modify existing message behavior.
* **Observability Standardization:** By following the W3C Trace Context format, the C++ services can extract `traceparent` and `tracestate` from the JSON envelope and reconstruct the parent context using standard OpenTelemetry APIs.

## Consequences

* **Positive:**  
  * Full alignment with the update to **ADR-003**, reusing 100% of the existing network parsing and buffer-handling code.
  * Complete end-to-end traceability visible in SigNoz under a single transaction tree.
* **Drawbacks:** The size of the JSON payload increases slightly due to the inclusion of telemetry strings, adding a marginal network byte overhead per analysis request.
* **Restrictions Introduced:**  
  * If a C client sends an `ANALYZE_GRAPH` message without the `traceparent` field, Server v0.1 is strictly required to generate a root context (*Root Trace*) before forwarding the packet to the HPC engine in order to prevent telemetry fragmentation.
  * Error codes defined in the technical document must be recorded as OpenTelemetry span error states and, where metrics are exported, reflected in the counters used by Grafana SLO dashboards.

## References
* `docs/adr/ADR-003-message-protocol-design.md` (Base Protocol and v0.2 Milestone Addendum)
* `docs/analyze-graph-protocol-v0.2.md` (Formal specification of contracts and JSON schemas)
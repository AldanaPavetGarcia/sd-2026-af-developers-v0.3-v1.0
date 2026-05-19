# ADR 005: Distributed Observability and Traceability Strategy

## Context and Problem

Since EOP is a distributed system, the lack of native instrumentation hinders fault diagnosis, limiting analysis to the manual inspection of isolated logs using `kubectl logs`. This approach is neither scalable nor does it allow for visualizing causality between services. Because adapting distributed tracing to an advanced codebase is significantly more expensive, a unified observability infrastructure needs to be designed and integrated from the **v0.1** phase to immediately answer: *“What happened, where, when, and which request caused this error?”*.

## Options Considered

### **Strategy and Component Stack**

1. **Custom Logging Framework:** Allows for structuring local messages, but does not provide correlation of distributed traces, keeping each service's logs in isolated silos.

2. **ELK Stack (Elasticsearch, Logstash, Kibana):** Highly powerful, but rejected due to its high operational load, resource consumption, and maintenance complexity in lab development environments.

3. **Jaeger as a Trace Backend:** Robust tracing tool, but requires complementary tools for metrics, fragmenting the user experience.

4. **OpenTelemetry SDK + SigNoz:** Use of an open instrumentation standard (OTel) coupled with a unified backend (SigNoz) that consolidates metrics and traces into a single interface.

### **Implementation Phase**

1. **Deferring Observability to v0.3:** Delays the complexity of the analysis, but drastically increases refactoring costs, losing end-to-end correlation from day one.

2. **Native Instrumentation from v0.1:** Integrates trace identifiers into the core of the communication system immediately.

## Decision

It has been decided to adopt **Native Instrumentation from v0.1 using the OpenTelemetry standard and SigNoz as the centralized backend** (Option 4). The deployment of the complete stack will be structured incrementally in layers:

* **v0.1 (Current):** Distributed Traces (OTel SDK + SigNoz) and Structured Logs in JSON.

* **v0.2:** Metrics and SLOs (Prometheus + Grafana).

* **v0.3:** Advanced Log Centralization (Loki + Grafana Explore).

## Justification (Trade-off Analysis)

* **Prevention of Vendor Lock-in:** The OpenTelemetry SDK ensures that the naming of spans, attribute keys, and error handling follow universal semantic conventions. Changing the backend in the future (e.g., migrating from SigNoz to Jaeger) will only require configuration modifications in the collector, without altering a single line of source code.

* **Native Correlation:** By unifying the `trace_id` across all languages ​​used (C++ and Go), it's possible to reconstruct the complete lifecycle of a request across service network boundaries.

* **Operational Simplicity:** SigNoz was selected over alternatives like ELK thanks to its integrated experience and optimized learning curve for the team's ecosystem, strictly meeting the need for agility in diagnostics.

## Consequences

* **Positive:** Drastic reduction in mean time to failure (MTTR); immediate visibility of latency bottlenecks; a technical foundation prepared for native log-to-trace indexing and correlation, which will be completed in version 0.3.

* * **Drawbacks:** Introduces overhead in the initial development of v0.1, making instrumentation a mandatory acceptance criterion (AC) rather than an optional one for stories like **US-110**.

* **Technical Restrictions and Invariants:**

* **Network Architecture:** The `otel-collector` must run as a separate container in `docker-compose.yml`. Configuring internal memory buffers is mandatory to prevent telemetry loss if SigNoz is temporarily offline.

* **Span Boundaries:** Each service must send a span for incoming (server) and outgoing (client) requests, as well as for critical failures (by explicitly setting the `status = ERROR` attribute and injecting the error message).

* * **Log Contract:** All logs must be written in a structured format to `stdout`, strictly adhering to the following JSON schema, invariably coupled to the active `trace_id`:
```json
{
"timestamp": "2077-01-01T00:00:00.000Z",
"level": "INFO",
"service": "eop-server-svc",
"trace_id": "4bf92f3577b34da6...",
"span_id": "00f067aa0ba902b7",
"message": "Node registered successfully",
"node_id": "bunker-42"
}

```

* **Security Policies:** It is strictly forbidden to dump confidential information (tokens, credentials or PII) into the span keys or log payloads.

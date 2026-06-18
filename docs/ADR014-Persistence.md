# ADR-014: Persistence of Logs in Structured Format (JSON)

## Context and Problem

The initial phase user stories (US-101, US-102, US-104) require comprehensive logging of connections, critical errors, commands, and disconnection events. To comply with US-110, the system must be able to export this data so that the OpenTelemetry and SigNoz collectors can efficiently process and index it. The use of traditional plain text logs (*Raw Logs*) prevents automated analysis and breaks the ability to correlate distributed events using unique identifiers.

## Options Considered

1. **Traditional Plain Text Logs:** Simple to implement and with low storage consumption, but impossible to parse automatically by observability tools without complex regular expressions.

2. **CSV Format (Comma Separated Values):** Structured and compact, but rigid in the face of schema changes and lacking native support for nesting telemetry metadata.

3. **Structured JSON Stream to Standard Output (`stdout`):** Universalized key-value format, directly processable by Kubernetes collection agents and OTel collectors.

## Decision

It has been decided to implement the **Structured JSON Stream to Standard Output (`stdout`)**, managed centrally by the `Logger` class (Option 3). Data segregation and differentiation will be performed at the schema level using the mandatory `"event"` field, classified into three logical categories:

* **`connections`:** Logging of handshakes, session assignments, and disconnections.
* **`errors`:** Protocol failures, network outages, and system exceptions.
* **`audit`:** History of which `node_id` executed which specific operation.

## Justification (Trade-off Analysis)

* **Standardization and Automatic Parsing:** JSON is the native format of modern observability. By outputting logs directly to `stdout`, the need to manage physical files on disk within the container is eliminated, delegating persistence and rotation to the Kubernetes infrastructure (in accordance with **ADR 006**).

* **Traceability and Cross-Correlation:** Enables the exact injection of the technical fields `trace_id` and `span_id` required by **US-110 AC4**, allowing SigNoz to deterministically link a log line with its corresponding trace.

* **Schema Flexibility:** If new metadata needs to be added per event in Lab 2 or Lab 3, the JSON format allows for the incorporation of new keys without breaking the existing monitoring platform parsers.

## Consequences

* **Positive:** Strict compliance with Vault-Tec operational audits; real-time error diagnosis; and a radical simplification of the `Logger` class design by eliminating the need to handle concurrent text file descriptors on disk.

* **Negative:** The JSON format generates greater serialization overhead in C++ and consumes more bandwidth/storage in the output buffer than plain text.
* **Technical Design Invariants:** The JSON object emitted by the `Logger` must have the following semantic structure on each line:
```json
{
"timestamp": "ISO-8601-FORMATED-STRING",
"level": "INFO|WARN|ERROR",
"service": "eop-server-svc",
"trace_id": "string",
"span_id": "string",
"node_id": "string",
"event": "connections|errors|audit",
"message": "Humanly description of the operation"
}

```

---

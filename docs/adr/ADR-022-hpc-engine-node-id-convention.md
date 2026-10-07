# ADR-022: HPC Engine Node_id Reservation Convention

## Context and Problem

**ADR-008** establishes that `hpc-engine` integrates into the network by acting as an **EOP Node**, registering itself with the v0.1 server through the existing `REGISTER` message, exactly like any bunker. **ADR-018** defines how regular bunker `node_id` values are generated and managed (client-managed hybrid identification with server-side reconnection logic).

Neither ADR specifies how the server dispatcher distinguishes a session that belongs to an `hpc-engine` instance from a session that belongs to a regular bunker. This distinction is not cosmetic: once a session is identified as an HPC Engine, the server must treat it differently from a bunker session in at least three ways:

1. **Dispatch eligibility** — only sessions recognized as an HPC Engine may be selected as the target of an `ANALYZE_GRAPH` forward (`findAvailableHpcEngineSession`).
2. **Idle-timeout exemption** — HPC Engine sessions must not be dropped by the idle-timeout monitor for lack of `HEARTBEAT` traffic while they are reserved for dispatch, since request/response cycles for graph analysis do not follow the bunker heartbeat cadence.
3. **Poll loop exclusion** — the server's main `poll()` loop must not attempt to read from an HPC Engine's socket directly once it is reserved for dispatch, since responses to forwarded `ANALYZE_GRAPH` requests are consumed synchronously by the dispatch path (`receiveHpcEngineResponse`) instead of the generic per-client read loop. Without excluding the fd, both paths would race to read from the same socket.

Without an explicit, documented convention, this becomes an undocumented architectural decision, which **ADR-008's own Engineering Principles reference (and the SW-Agreement, §2.2)** explicitly prohibits: *"No undocumented architectural decisions."*

## Decision

The EOP server identifies an HPC Engine session by a **reserved `node_id` naming convention**, checked at `REGISTER` time:

- The literal value `hpc-engine` identifies a single HPC Engine instance.
- Any value matching the prefix `hpc-engine-<suffix>` (e.g. `hpc-engine-1`, `hpc-engine-gpu-a`) identifies one instance among multiple horizontally-scaled HPC Engine replicas.

Any `node_id` matching either form is **reserved**: it must not be used by a regular bunker node, and the server treats a session registered under it as dispatch-eligible (`reservedForDispatch = true`), exempt from idle-timeout disconnection, and excluded from the generic per-client poll loop for as long as the session is active.

No new protocol message or payload field is introduced. Detection happens purely by inspecting the `node_id` string supplied in the existing `REGISTER` message, reusing 100% of the registration, uniqueness-checking, and reconnection logic already defined by ADR-018.

## Alternatives Considered

| Alternative | Reason for Rejection |
| --- | --- |
| **Explicit role field in `REGISTER`** (e.g. `role=hpc-engine` alongside `node_id`) | More explicit and less prone to accidental collision, but requires extending the `REGISTER` payload schema — a protocol change that ADR-015 deliberately avoided for `ANALYZE_GRAPH` itself in favor of additive, non-breaking changes. Introducing a schema change for engine identification specifically would create two different extension mechanisms for the same milestone without a strong justification. |
| **Separate message type** (e.g. `REGISTER_ENGINE` instead of reusing `REGISTER`) | Directly contradicts ADR-008's stated justification for reusing the `REGISTER` flow ("Reuse of Infrastructure: by forcing the engine to connect via the `REGISTER` message, we reuse 100% of the socket framing, error handling, and connection state machines developed in v0.1"). |
| **Single hardcoded `node_id` only (`hpc-engine`, no prefix form)** | Simpler, but forecloses the "Horizontal pods" execution mode that ADR-017 explicitly requires for the Persistent Deployment Mode of `hpc-engine`. A fixed single id cannot support more than one concurrently registered engine replica. |
| **Out-of-band engine registry** (a separate config file or environment variable on the server listing known engine ids) | Rejected because it duplicates state that already lives in the `NodeRegistry`, requires manual synchronization between server configuration and actual deployed engine replicas, and does not scale automatically with Kubernetes pod naming (ADR-017). |

## Justification (Trade-off Analysis)

* **Zero protocol footprint**: the convention is enforced entirely server-side, by string inspection at `REGISTER` time. It requires no changes to the wire protocol, to `docs/analyze-graph-protocol-v0.2.md`, or to the C client library.
* **Compatible with horizontal scaling**: the `hpc-engine-<suffix>` form allows multiple engine replicas to register concurrently under distinct, colliding-free identifiers, satisfying the "Horizontal pods" mode described in ADR-017 without any further protocol work.
* **Reuses existing guarantees**: duplicate-registration handling, reconnection, and session-to-node binding all continue to be governed by ADR-018's existing rules — the reserved naming convention only changes how the resulting session is *treated* after registration, not how registration itself is validated.
* **Minimal, isolated implementation surface**: the entire decision is implemented behind a single predicate (`isHpcEngineNodeId`), keeping the blast radius of the convention small and easy to audit or change later.

## Consequences

* **Positive**: no protocol or client-library changes required; supports single-instance and horizontally-scaled engine deployments identically; keeps the dispatch, idle-timeout, and poll-loop special-casing centralized behind one naming check.

* **Drawbacks**: the convention is a **soft contract** enforced only by string matching, not by a distinct schema field. Nothing prevents a misconfigured bunker from accidentally registering with a `node_id` of `hpc-engine` or `hpc-engine-<suffix>`, which would cause the server to treat a regular bunker as a dispatch target for `ANALYZE_GRAPH` requests.

* **Restrictions Introduced**:
  * `node_id` values matching `hpc-engine` or `hpc-engine-<suffix>` are **reserved** and must not be assigned to bunker nodes. This restriction should be reflected in operator-facing documentation and, where practical, validated during bunker provisioning.
  * Any future change to how HPC Engine instances are named (e.g. adopting Kubernetes pod names directly) must update `isHpcEngineNodeId` and this ADR together, to avoid re-introducing an undocumented convention.

## References

* `ADR-008-hpc-engine-integration-pattern.md` — establishes that `hpc-engine` registers via the existing `REGISTER` flow as an EOP Node.
* `ADR-017-kubernetes-workload-management.md` — requires support for horizontally-scaled `hpc-engine` replicas under the Persistent Deployment Mode.
* `ADR-018-node-id-assignment.md` — governs `node_id` generation, uniqueness, and reconnection for regular bunker nodes; this ADR extends that model with a reserved namespace rather than replacing it.
* `src/app/eopServer.cpp` — `isHpcEngineNodeId`, `findAvailableHpcEngineSession`, `bindNodeToSession` (`reservedForDispatch` assignment).

# ADR-013: Node_id Assignment Strategy and Reconnection Management

## Context and Problem

The EOP server needs to uniquely identify each bunker that connects to the network. The technical challenge lies in deterministically differentiating between a "First Connection" and a "Reconnection" after a client failure or a Pod reboot (US-109). Without a clear strategy, the server risks duplicating entries in the in-memory registry (`NodeRegistry`), violating the acceptance criterion **US-103 AC3** (`ERROR_DUPLICATE`), or losing the node state history when restoring the session.

## Options Considered

1. **Centralized Assignment by the Server:** The server generates the `node_id` sequentially or randomly and delivers it in the `ACK`. (Rejected: If the client disconnects abruptly and loses state, it has no way to reclaim its previous identity, forcing an orphaned re-registration.)

2. **Static Generation by Configuration:** Each bunker has a hardcoded `node_id` in its local configuration file. (Rejected: Reduces Vault-Tec's operational flexibility and hinders dynamic scaling in automated environments.)

3. **Client-Managed Hybrid Identification with Local Persistence:** The client generates its initial identifier and is responsible for persisting it. Upon reconnection, it reuses this identity to synchronize state on the server.

## Decision

It has been decided to implement the **Client-Managed Hybrid Identification System with server-side restore logic** (Option 3).

## Justification (Trade-off Analysis)

* **Autonomous State Restoration:** Ensures strict compliance with **US-104 AC5**. If a node goes `OFFLINE` after missing a heartbeat (`EOP_HEARTBEAT_INTERVAL`), the server does not destroy its metadata; upon receiving the same `node_id` in a subsequent attempt, it transparently switches the state back to `ONLINE`.

* **Base Decoupling:** The server's validation logic is simplified. It does not need to coordinate the generation of global IDs at this stage, delegating the responsibility for initial uniqueness to the C integration library (Epic E2).

* **History Consistency:** Ensures that traces sent to **SigNoz (US-110)** maintain semantic consistency, as operations from the same physical bunker will be grouped under the same `node_id` over time, facilitating diagnostics in the Runbook.

## Consequences

* **Positive:** Hot recovery from failures without manual operator intervention; helps prevent memory leaks from orphaned records in the server's internal map by reusing node allocations upon reconnection.

* **Negative:** Introduces a critical dependency on the client's entropy generator. There is a marginal risk of ID collisions if random generation is weak.

* **Technical Implementation Rules:**

* **Server Logic:** Upon receiving a `REGISTER` message, if the `node_id` exists and is `OFFLINE`, the `session.id` and IP address are updated, and `ONLINE` is set. If it exists and is already `ONLINE`, it is rejected with `ERROR_DUPLICATE`. If it does not exist, it is created as a new entry.

* **Collision Mitigation:** The C library must generate identifiers with a robust entropy rate (UUID v4 or high-dispersion long hash) to guarantee probabilistic uniqueness.

* **Architecture Coupling:** The class responsible for connection monitoring (`Watchdog` / `SelectLoop`) must be able to dynamically reassociate the new TCP socket file descriptor with the pre-existing `node_id` in the session record.

--

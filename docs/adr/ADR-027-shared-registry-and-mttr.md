# ADR-027: Shared Node Registry and Pod-Crash MTTR by Replica Failover

**Status:** Accepted (v1.0, issue #66). Implements the v1.0 path chosen in ADR-025 (option C and
measurement M4). Implemented by #71; demonstrated live by US-407 AC4.

## Context and Problem

The PRD's reliability NFR asks for **MTTR on pod crash < 200 ms**, shown in Grafana, and US-407
AC4 demonstrates it live with `kubectl delete pod`. ADR-025 (course repository, `ADR020-Pod-Crash-MTTR.md`)
measured the single-replica floor: with probes tuned, a replacement Pod is Ready ~2–3 s after the
crash, and nothing in Kubernetes takes recreation below ~2 s. Its conclusion: 200 ms is reachable
only as **service restoration by a replica that is already running**, which needs the node
registry outside the server process (its option C), measured with millisecond resolution from
the Kubernetes API (its option M4).

Today `eop-server` keeps `NodeRegistry` (`include/eopServer.hpp`) in process memory. With two
replicas behind the Service, `LIST_NODES` would show half the nodes, `QUERY_NODE` would depend on
which replica answered, and `ERROR_DUPLICATE` (US-103 AC3) would not be detected across replicas
(ADR-025, option B).

Two more facts shape the design:

* **Sessions are connection-bound** (ADR-018): a node is `ONLINE` while the TCP session that
  registered it is alive, and goes `OFFLINE` when it closes or misses heartbeats. A session lives
  on exactly one replica.
* **`hpc-engine` registers as a peer** under a reserved `node_id` (ADR-022) and receives
  `ANALYZE_GRAPH` dispatches over its own session. With two server replicas, each one needs an
  engine session of its own.

## Decision

### 1. Registry behind an interface, two backends

`INodeRegistry` keeps today's operations (`registerOrReconnectNode`, `queryNode`, `listNodes`,
heartbeat and offline transitions) with two implementations selected by
`EOP_REGISTRY_BACKEND=memory|redis`:

* `InMemoryNodeRegistry` — today's code, unchanged behaviour. Default, used by unit tests,
  docker-compose and single-replica deployments.
* `RedisNodeRegistry` — hiredis (vcpkg), wrapped in RAII types for the connection and replies;
  errors are returned as codes, never thrown across the module boundary.

### 2. Data model and ownership

* One hash per node, `eop:node:{node_id}`: the v0.1 fields, `source` (`tcp` or `edge`, ADR-028),
  and `owner` — the name of the replica holding its session (Pod name from the downward API).
* One lease per replica, `eop:replica:{pod}`, with a 3 s TTL refreshed every second.
* A node's **effective status** is `ONLINE` only if its stored status is `ONLINE` **and** its
  owner's lease exists. A replica that dies stops refreshing its lease, so within 3 s all its nodes
  read as `OFFLINE` everywhere, without anyone writing to them.
* `REGISTER` runs as one Lua script (atomic in Redis): new node → created; node `OFFLINE` or whose
  owner lease is gone, same metadata → reconnected and taken over by the caller's replica; node
  effectively `ONLINE` elsewhere → `ERROR_DUPLICATE` (US-103 AC3 across replicas).
* Each replica keeps a local copy of the nodes it owns and writes them again on heartbeat and when
  its Redis connection comes back. Registry keys have no TTL, so the `volatile-ttl` eviction policy
  never removes them; Redis stays without persistence (ADR-010 §5) because the registry is soft
  state that live sessions rebuild after a Redis restart.

### 3. Two replicas, engine as a sidecar

* `eop-server` runs **2 replicas** with a PodDisruptionBudget (`minAvailable: 1`), the
  `startupProbe` of ADR-025 and readiness that includes the registry backend.
* `hpc-engine` moves into the server Pod as a **sidecar** and registers with `localhost` as
  `hpc-engine-<pod>` (ADR-022's multi-instance form). Engine sessions stay in the replica's local
  table and are not written to Redis: each replica dispatches to its own engine, and a Pod's death
  takes both down together, which the other Pod covers.

### 4. What MTTR means here, and how it is measured

* **MTTR (service restoration):** from the crash of an `eop-server` Pod to the moment the Service
  no longer routes to it while at least one other replica is Ready. That is when a new client is
  served again; it is what the 200 ms target is checked against.
* **Replica replacement time:** pod death → replacement Pod Ready, the PRD's literal wording. It
  stays ~2–3 s (ADR-025) and is shown **next to** MTTR, labelled as what it is.
* **`mttr-watcher`** (Go, `services/go/cmd/mttr-watcher`), ADR-025's M4: watches the server's Pods
  and EndpointSlices through the Kubernetes API (namespaced `Role`, `list`/`watch` only), stamps
  every event with millisecond precision on arrival, and exports both durations to Prometheus for
  the Grafana MTTR panel. It is event-driven and sends nothing to `eop-server`, which answers
  ADR-025's objection to a permanent prober.
* **Client-side evidence:** `make k8s-mttr-test` deletes a server Pod while ADR-025's synthetic
  prober (20 ms, run only during the test) connects through the Service, and fails if either the
  prober's outage or the watcher's MTTR is ≥ 200 ms. The prober also covers what the watcher
  cannot see: the time between a hard crash and the kubelet reporting it.

### 5. Clients across a failover

* API gateway: its pool already discards a broken connection and retries reads on a new one
  (ADR-010); the new connection lands on the live replica.
* Bunker nodes (`libeop`) and the engine: their session dies with the Pod. A node reconnects and
  re-registers on the other replica as soon as it notices; until then (≤ 3 s, the lease) it reads
  as `OFFLINE`. That is the node's own recovery, not the service's.

## Alternatives Considered

| Option | Why not |
|---|---|
| One replica, faster restart | ADR-025: floor of ~2 s |
| Several replicas without shared state | ADR-025 option B: wrong `LIST_NODES`, cross-replica duplicates |
| Leader election (Kubernetes `Lease`) | ADR-025 option D: seconds to fail over, and still needs state handover |
| Registry in PostgreSQL / etcd | A new stateful component for data Redis (already deployed in v0.3) holds well; etcd is the cluster's own store, not an application database |
| Keep the engine as a separate Deployment | Each replica needs its own engine session; one engine cannot hold sessions on two servers without changing ADR-008's connection model |
| Continuous synthetic prober as the MTTR source | ADR-025: ~3.4 M log lines and ~1.7 M spans a day on `eop-server` |

## Consequences

* **Positive:** a Pod crash is absorbed by the other replica in well under a second; the registry
  stays correct across replicas; the panel shows both numbers honestly.
* **Negative:** Redis becomes a dependency of `eop-server` itself (with `EOP_REGISTRY_BACKEND=redis`);
  a Redis outage makes the server not Ready, so Redis restart time is now part of the server's
  availability. Two server Pods and two engines double their resource requests (the 7 GB laptop
  profile keeps one replica with the memory backend). The registry path gets a network round trip.
* **To confirm with the course:** this ADR reads "MTTR < 200 ms" as service restoration. The
  PRD's literal *pod death → pod ready* cannot reach 200 ms on Kubernetes (ADR-025) and is shown
  next to it.

## Sequence: failover

```
client      Service        server-A (dies)     server-B          Redis        mttr-watcher
  │ QUERY ──►│──────────────►│ ✗ crash                                           │
  │          │  endpoint A not ready ◄──────────────────────── Pod / EndpointSlice events
  │ retry ──►│───────────────────────────────►│ HGET node ─────►│               │ MTTR = t(A removed) − t(crash)
  │ ◄────────────────────────── answer ───────│                 │               │
  │          │                                │ lease A expired → A's nodes read OFFLINE (≤ 3 s)
  │ (node) REGISTER ─────────────────────────►│ Lua: take over ─►│
```

## References

* ADR-025 (course repository `ADR020-Pod-Crash-MTTR.md`, PR #68) — measurements and options.
* ADR-018 — sessions and reconnection; ADR-022 — engine `node_id`; ADR-008 — engine integration.
* ADR-010 §5 — Redis deployment; `docs/release-0.2.0-known-limitations.md` §3.
* PRD — NFR "MTTR on pod crash", US-407 AC4.

# ADR-009: Graph Representation Data Structure

## Context and Problem

The `hpc-engine` must ingest external network topologies containing up to **100,000 nodes** and execute concurrent algorithms under strict latency constraints. The selection of the in-memory representation of the graph directly governs the execution efficiency of parallel Dijkstra and centralities, as well as the physical memory footprint allowed within the deployment container.

## Decision

The `hpc-engine` will completely avoid dense structures and will internally represent the topology using an optimized **Adjacency List** utilizing contiguous memory vectors (`std::vector<std::vector<Edge>>`), mapped directly from an initial string-to-index translation dictionary during the ingestion phase.

```
[Node Index] ---> [std::vector<Edge>] (Contiguous Neighbors in Memory)
    [0]      ---> {Target: 1, Weight: 10} -> {Target: 2, Weight: 5}
    [1]      ---> {Target: 2, Weight: 12}
```

## Alternatives Considered

| Structure | Memory Complexity | Time Complexity (Iterate Neighbors) | Reason For Rejection |
| --- | --- | --- | --- |
| **Adjacency Matrix** | $O(V^2)$ | $O(V)$ | **Fatal Spatial Footprint.** For $100,000$ nodes, a dense matrix requires $100,000 \times 100,000 \times 4 \text{ bytes} \approx 40 \text{ GB}$ of contiguous RAM. This exceeds the target Kubernetes cluster node capacity and triggers instant Out-Of-Memory (OOM) kills. |
| **Raw Edge List Pointer Arrays** | $O(E)$ | $O(E)$ | Poor cache locality. Searching the neighbor of a specific vertex requires scanning the entire edge array, ruining parallel loop performance. |


## Justification (Trade-off Analysis)

Bunker network topologies are inherently **sparse graphs** (each node connects to a localized subset of neighboring installations, meaning $E \ll V^2$). For sparse bunker topologies, adjacency lists provide $O(V + E)$ memory usage, avoiding the $O(V^2)$ memory cost of adjacency matrices. Exact memory consumption depends on the number of edges and the concrete `Edge` representation, leaving the processor cache lines open for algorithm execution.


## Consequences

* **Positive:** Ultra-low memory consumption; high cache-line hit ratios when iterating through immediate neighbor weights in OpenMP loops.

* **Drawbacks:** Vertices must be mapped from alphanumeric strings (e.g., `"bunker-alpha"`) to sequential integer indices ($0$ to $N-1$) upon receipt. This introduces a preprocessing step to map external string identifiers to internal numeric indices.

* **Restrictions Introduced:** Strict validation is enforced at the boundary. If any edge payload points to a vertex index or identifier outside the verified node allocation array, the parser must abort processing immediately and emit an `ERROR_INVALID_GRAPH` code, guaranteeing memory isolation.

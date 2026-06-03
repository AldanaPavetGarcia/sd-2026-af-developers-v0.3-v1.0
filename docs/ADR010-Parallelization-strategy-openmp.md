# ADR-010: Parallelization Strategy — OpenMP for Graph Algorithms

## Context and Problem

To satisfy the performance criteria defined by Vault-Tec, the engine must process network topologies with a clear acceleration factor matching the available computational hardware. For instance, a graph of $\ge 50,000$ nodes must scale with an acceleration factor of $\ge 2\times$ when allocating 4 cores, and scale to $\ge 3\times$ on graphs of $\ge 100,000$ nodes using 8 cores. The team needs a framework that provides high-throughput loop parallelization without expanding codebase debt or manual scheduling overhead.


## Decision

The engine will use **OpenMP** (Open Multi-Processing) as its primary shared-memory parallelization mechanism. Parallelism will be achieved through explicit compiler directives targeting loop-level work distribution combined with dynamic scheduling to prevent thread starvation on irregular graph structures.

```cpp
void compute_centrality(const Graph& graph, std::vector<double>& results) {
    const int num_nodes = graph.get_vertex_count();
    
    #pragma omp parallel for schedule(dynamic) num_threads(omp_get_max_threads())

    for (int i = 0; i < num_nodes; ++i) {
        // High-performance thread-isolated computation region
        execute_brandes_iteration(graph, i, results);
    }
}
```


## Alternatives Considered

* **Manual POSIX Threads (`pthread`) or `std::thread`:** Rejected due to excessive implementation complexity. Writing a custom thread pool with work-stealing queues to handle unbalanced graph branches introduces significant maintenance overhead and increases the surface area for deadlocks.

* **CUDA / GPU Parallelism:** Provides massive acceleration, but introduces a hard hardware dependency on specialized graphics cards. This violates requirements for local developer environment portability and uniform bunker deployments.


## Justification (Trade-off Analysis)

OpenMP is built directly into the toolchain compilers (GCC and Clang). It requires no external runtime dependency injection, operates natively on shared memory, and shifts the responsibility of work distribution and load balancing entirely to the compiler via basic, testable pragmas.


## Consequences

* **Positive:** Direct runtime execution control through the standard `OMP_NUM_THREADS` environment variable without requiring source re-compilation.

* **Drawbacks:** Thread synchronization bugs (data races) become a critical risk. Concurrent vector accumulation requires explicit use of OpenMP allocation reduction clauses (`reduction(+:variable)`) or local atomic variables.

* **Restrictions Introduced:**

* The automated CI pipeline must validate all test execution routines using **ThreadSanitizer** (`-fsanitize=thread`). The build will reject any code change triggering non-zero concurrency warnings.

* The engine must strictly honor system resource allocation; it is explicitly forbidden to spawn or utilize more concurrent CPU threads than defined inside the active `OMP_NUM_THREADS` variable. This behavior must be verified using system monitoring utilities during regular benchmark validation routines (`make benchmark`).

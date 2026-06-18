# ADR 002: Server Concurrency Model — Thread Pool

## Context and Problem

The C++ EOP server must support a minimum of 100 concurrent clients (NFR-1) processing messages asynchronously to avoid connection blocking. The choice of concurrency model directly affects the memory footprint per client, implementation complexity, and stability under load. A balance is sought between efficient use of operating system resources and code maintainability without resorting to the extreme complexity of a pure state machine.

## Options Considered

1. **Thread-per-connection:** A thread is created for each client. It is simple to implement but consumes too much stack memory (2–8 MB per thread) and degrades performance due to context switching overhead with 100+ clients.

2. **Thread Pool:** A fixed group of worker threads processes queued requests. It allows for predictable memory usage and decouples data reception from logical processing.

3. **I/O Multiplexing (epoll):** A single thread monitors thousands of descriptors using events. It offers maximum scalability but drastically increases complexity by requiring fragmented logic in callbacks and state machines.

## Decision

The **Thread Pool** model (Option 2) has been chosen for implementation.

## Justification (Trade-off Analysis)

* **Memory Predictability:** By maintaining a fixed number of threads, stack memory consumption is limited and independent of the number of connected clients, complying with the environment's resource constraints.

* **Scalability and Performance:** It is superior to the "one thread per connection" model for expected traffic volumes. It allows adjusting the pool size according to the number of available physical cores, optimizing actual hardware performance.

* **Moderate Complexity:** Although it requires the implementation of a thread-safe queue, it allows for more linear and easier-to-debug processing logic than the pure asynchronous (epoll) model.

* **Technical Balance:** It is the ideal model for complying with AC2 (latency < 2x under a load of 100 clients) without incurring the excessive development time of event multiplexing.

## Consequences

* **Positive:** System stability under message bursts, efficient CPU utilization, and a robust infrastructure for passing load tests (US-102).

* **Negative:** It introduces the need for robust synchronization primitives. Potential bottlenecks in the shared queue must be carefully managed.

* **Technical Implementation:**
* **Mutexes** and **Condition Variables** will be used for synchronization between the producer thread (the one that accepts connections/messages) and the consumer threads (workers).

* Shared state (such as the node registry) will be protected using granular locks to minimize contention.

* The maximum client limit (`EOP_MAX_CLIENTS`) will be configurable to prevent file descriptor exhaustion.

* The absence of race conditions will be validated using **ThreadSanitizer** (AC6).

---

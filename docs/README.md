# System Architecture & Operations Manual (EOP System v0.1)

This document describes the comprehensive system architecture, modular design, and operational procedures implemented by the engineering team for the **EOP Protocol Ecosystem**.

---

## 1. System Overview & Service Scope

The system is designed as a high-performance, low-latency communication ecosystem connecting distributed bunker nodes to a centralized infrastructure. Below is the scope defined by Epics.

### Epic E1: C++ TCP Server (Core Infrastructure)

* **Connection and Greeting:** The server opens a passive socket on port `EOP_PORT` (9026). Upon detecting a connection (`accept`), it initiates a welcome protocol by sending a structured message with the server version, a Unix timestamp, and a unique `session_id`. It manages a 30s inactivity timeout.
* *What it does NOT do:* It does not implement authentication (user/pass). It does not encrypt data (plain text). It does not handle multiple protocol versions simultaneously.


* **Concurrent Multi-client Support:** Implements a concurrency model designed to ensure that up to 100 bunkers operate without cross-latency. If a bunker dies (`SIGKILL`), the server safely releases the file descriptor without affecting the others.
* *What it does NOT do:* It does not scale beyond a single CPU instance (no load balancing between servers). It does not guarantee extreme fairness if a client intentionally saturates bandwidth.


* **Centralized Node Registry:** Processes the `REGISTER` message. Stores `node_id`, `bunker_name`, `ip_address`, and `capacity` in an in-memory data structure (`std::unordered_map`). Allows individual queries (`QUERY_NODE`) and full listing (`LIST_NODES`).
* *What it does NOT do:* It does not persist data (volatile memory). It does not allow partial metadata updates (requires re-registration).


* **Failure Detection and Offline Status:** If no traffic or heartbeat is received from the client within the `EOP_HEARTBEAT_INTERVAL`, the server marks the node as `OFFLINE` in the registry.
* *What it does NOT do:* It does not attempt to "reconnect" to the client from the server. It does not remove the node from the registry, it only changes its status label.



### Epic E2: C Client Library (Integration SDK)

* **Connection Abstraction:** Provides the function `eop_connect(host, port)`. It encapsulates socket creation, DNS resolution, and initial greeting handling, returning an opaque pointer `eop_client_t*`.
* **Command-Response Cycle:** Exposes `eop_send_command`. It is a synchronous, blocking function that sends command bytes, waits for the server response, and returns an `eop_response_t` structure. Handles internal timeouts to prevent hanging.
* *What it does NOT do:* It does not support asynchronous commands (callbacks). It does not fragment large messages.


* **Resource Cleanup:** The `eop_disconnect` function ensures the socket is closed using an exit handshake (`SHUT_RDWR`), notifying the server, and freeing all dynamic heap memory.

### Epic E3: Containerization and Kubernetes (Orchestration)

* **Reproducible Deployment:** Multi-stage Docker builds (compiles in a heavy image, runs in an Alpine/Distroless image). The server handles `SIGTERM` signals for a 15-second "Graceful Shutdown."
* *What it does NOT do:* It does not include debugging tools (`gdb`, `vim`) in the production image to keep the size under 50MB.


* **Pod Self-healing:** Defines Liveness and Readiness probes. If the server process hangs (deadlock), K8s automatically restarts the pod. The service is marked as "Not Ready" during startup until port 9026 is fully open.

### Epic E4: Basic Observability

* **Tracing with SigNoz:** Instruments the C++ code with the OpenTelemetry SDK. Each incoming command creates a "Span" sent to the collector to measure operation latency. Correlates JSON logs with trace IDs.
* *What it does NOT do:* It does not measure detailed bunker hardware metrics (CPU/RAM) or provide business alerts.

### Epic E5: Parallel HPC Engine (`hpc-engine`)

* **Graph Analysis:** A standalone C++/OpenMP binary that runs `shortest_paths` (parallel Dijkstra), `connected_components`, and `centrality` over a JSON graph payload, per the `ANALYZE_GRAPH` contract in `docs/analyze-graph-protocol-v0.2.md`.
* **Two execution modes:** invoked with `--server-host`/`--server-port`/`--node-id`, it opens a persistent socket, `REGISTER`s with the v0.1 server as a peer, and keeps servicing `ANALYZE_GRAPH` requests indefinitely (with bounded connect/register retry-backoff and a heartbeat file for external liveness checks). Invoked without those flags, it falls back to a one-shot batch mode: read one request from stdin, write one response to stdout, exit.
* *What it does NOT do:* It does not accept direct client connections — all requests reach it dispatched through the v0.1 server (`eopServer.cpp`), never directly from a bunker.

### Epic E6: Kubernetes Workload Management

* **Dual-Execution Vector Deployment:** `hpc-engine` is deployed both ways described above — as a long-lived `Deployment` (`k8s/hpc-engine-deployment.yaml`, persistent socket mode, `replicas: 1` — the server's dispatch mutex serializes requests to a single engine today) and as a one-shot `Job` (`k8s/jobs/hpc-engine-batch-job.yaml`, batch mode) — matching the pattern chosen in `docs/ADR011-Kubernetes-workload-management.md`.
* **Probes, limits, and config:** the Deployment defines a liveness probe (process-alive check), a readiness probe (heartbeat-file freshness, since `hpc-engine` exposes no listening port to probe), CPU/memory requests and limits, and reads its configuration from the shared `eop-global-config` ConfigMap (also used by `eop-server`).
* *What it does NOT do:* No `Service` fronts `hpc-engine` — it is an outbound-only TCP client, never a listener, so there is nothing to load-balance or route to. See `docs/kubernetes-deployment-and-pod-recovery-guide.md` for the full build/deploy/validate walkthrough of both workloads.

---

## 2. Class Structure and Modularity

The codebase is split into modular components to enforce a clean separation of concerns:

```
[ Client Nodes (C SDK) ] <--- network ---> [ Server Core (C++) ]
                                                   │
         ┌───────────────────┬─────────────────────┼─────────────────────┐
         ▼                   ▼                     ▼                     ▼
┌─────────────────┐ ┌─────────────────┐ ┌────────────────────┐ ┌───────────────────┐
│   WDog Module   │ │   Common (.so)  │ │ Communications(.so)│ │  Logger Module    │
│  Timer/Keepalive│ │  Enums/Contracts│ │ Dispatcher/Business│ │ Multi-Sink JSON   │
└─────────────────┘ └─────────────────┘ └────────────────────┘ └───────────────────┘

```

1. **Clients and Server (Core Entities):**
* **Server (C++):** Main class responsible for the lifecycle of the master socket (`bind`, `listen`, `accept`). Manages the Thread Pool and message orchestration.
* **Client (C):** High-level abstraction for bunker nodes. Encapsulates the file descriptor and session state.


2. **WDog (Watchdog & Timer Manager):** Monitors Keepalive and Heartbeat intervals. Maintains a record of "last activity" per `node_id`. If the current time exceeds timeouts, it triggers a forced disconnection event.
3. **Common (Shared Library - `.so`):** Acts as the **Single Source of Truth (SSOT)**. Contains definitions for `NODE_STATE` (`ONLINE`, `OFFLINE`, `REGISTERING`) and structured message types (Headers, command codes).
4. **Communications (Command Handler Library - `.so`):** Implements the **Dispatcher**. Receives a raw command, identifies its type, and calls the corresponding service function (e.g., `handle_register`, `handle_query`), decoupling transport from the application layer.
5. **Logger (Structured Multi-Sink Logging):** Implements differentiated destinations:
* `connections.json`: Tracks IPs, session IDs, and connection timestamps.
* `errors.json`: Detailed trace of internal failures and exceptions.
* `audit.json`: Log of execution commands paired with OTel `trace_id` correlation.



---

## 3. Environment Setup & Node Initialization

This section details the standardized procedures for initializing the local environment and establishing client-server communication.

### Step 1: Environment Construction (Build Process)

Compile the binaries using CMake from the project root directory:

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
cd ..

```

### Step 2: Node Initialization

To validate the bidirectional ecosystem, instantiate each process in a separate terminal or session.

#### Server Node (Terminal 1)

Deploys the main listener. Operating ports and idle timeouts are loaded from the `config.env` configuration file.

```bash
./build/eop_server

```

* **Expected Output State:**
`Server started on port 9026. Inactivity timeout: 60s.`

#### Client Node (Terminal 2)

Set the network socket pointing to the local server host and port:

```bash
./build/eop_client 127.0.0.1 9026

```

* **Expected Output State:**
`Connection established with 127.0.0.1:9026.`

---

## 4. Available Command Protocol (CLI Options)

Once the network layer is established on the client interface (**Terminal 2**), use the following interactive protocol instructions to communicate with the server registry.

### A. Initial Registration (REGISTER)

The initial session binding is completed atomically in the first registration transaction.

* **Command Format:**
```text
REGISTER node_id=alpha-01 bunker_name=Refugio-A ip_address=10.0.0.5 capacity=80

```


* **Server Response:** `ACK Server v1.0 | TIME <timestamp> | SESSION_ID alpha-01`
* **Server Audit (Terminal 1):** Internally records the session: `Total connections: 1`.

### B. Duplicate Prevention (ERROR_DUPLICATE)

The system safeguards registry integrity by ensuring the strict uniqueness of the `node_id` field.

* **Command to Inject:**

```text
    REGISTER node_id=alpha-01 bunker_name=Zone-B ip_address=10.0.0.6 capacity=10
    ```
*   **Server Response:**
    
```text
    ERROR_DUPLICATE node_id=alpha-01 desc=Node already registered
    ```

### C. Specific Query (QUERY_NODE)
Extracts the persisted attributes of an active node under its specific ID.
*   **Command to Inject:**
    
```text
    QUERY_NODE node_id=alpha-01
    ```
*   **Server Response:**
    
```text
    node_id=alpha-01 bunker_name=Refugio-A ip_address=10.0.0.5 capacity=80
    ```

### D. Global Query (LIST_NODES)
Iterates over the data structure to return all active linked elements.
*   **Command to Inject:**
    
```text
    LIST_NODES
    ```
*   **Server Response:** Emits a total count (`LIST_NODES count=X`), details all retrieved entries, and closes the transfer block using `END_LIST`.

### E. Explicit Termination (EXIT/QUIT)
Connection closure managed directly by the client.
*   **Command to Inject:**
    
```text
    exit
    ```
*   **Server Response:** The server asynchronously closes the descriptor and logs the controlled decrease: `Total connections: 0`.

---

## 5. Lifecycle, Performance & Validation Benchmarks

### Inactivity Monitoring
*   **Timeout (Idle Timeout):** If the client does not transmit frames within 60 seconds (parameter `EOP_IDLE_TIMEOUT`), the connection expires and is released. The server terminal will validate the event by issuing an idle timeout log.
*   **Auditing:** Refer to the analytical file `system.log` mounted in the root directory during the test routine to trace edge resolutions.

### Performance Benchmarking (Latency Testing)
To audit the efficiency of the transport layer under load, run the Python telemetry script. Ensure the server is operating in a clean state (Terminal 1).

From the project root (**Terminal 2**), run:
```bash
python3 tests/load/latency_check.py

```

The script will report metrics regarding **Min/Max Latency**, **Average RTT**, and **Packet Loss**.

### File Descriptor Leak Validation

Validates descriptor stability through repeated `connect -> register -> disconnect` cycles to avoid resource exhaustion under high connection churn.

Run the validation script using the following parameters from the project root:

```bash
# Test 10,000 cycles for connection and registration
python3 tests/load/<fd_leak_script>.py 10000 register

# Test 10,000 cycles for pure connection/disconnection
python3 tests/load/<fd_leak_script>.py 10000 connect

# Aggressive stress validation run (1 Million cycles)
python3 tests/load/<fd_leak_script>.py 1000000 register

```

* **Expected Successful Result:**
```text
PASS: No significant file descriptor leak detected

```


* **Failure Result:**
```text
FAIL: Possible file descriptor leak detected

```

---

### Observability Runbook (SigNoz to Logs)

This section provides the procedure to navigate from a trace in SigNoz to its corresponding JSON log entry on the server using `node.id`.

#### Context Correlation
The EOP server emits structured JSON logs to `stdout`. Every log line generated during an operation includes a `trace_id` and a `span_id`.

**Prerequisite:** Ensure the Docker Compose stack is running and the SigNoz dashboard is accessible at `http://localhost:8081`.

#### Step 1: Locating the Node in SigNoz
1. Open the **SigNoz UI**.
2. Navigate to the **Traces** tab.
3. In the filters bar, search for the `node.id` tag (e.g. `node.id="alpha-01"`).
4. Click on a specific trace to open the Trace Detail view. This view displays the waterfall of spans (e.g. `client_connect` -> `REGISTER`).

#### Step 2: Extracting Trace Information
1. On the trace timeline, click on the specific span you are investigating (e.g. the `REGISTER` span).
2. The right sidebar will open displaying the **Span Attributes**.
3. Note down the **Trace ID** and **Span ID** shown in the properties.

#### Step 3: Finding the Log Line
1. Connect to the host running the `eop-server` Docker container.
2. Search the standard output (or container logs) filtering by the `trace_id` or `node_id`.

Using `grep`:
```bash
# Filter by Trace ID:
docker logs eop-server | grep '"trace_id":"<YOUR_TRACE_ID>"'

# Filter by Node ID:
docker logs eop-server | grep '"node_id":"alpha-01"'
```

You will see the structured JSON output corresponding to that exact span, containing the specific event message and severity level, which allows for precise debugging.

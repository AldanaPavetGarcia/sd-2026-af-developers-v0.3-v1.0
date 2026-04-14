# System Architecture

This document should describe the architecture of the system implemented by the team.

Recommended sections:

- System overview
- Components
- Data flow
- Deployment architecture

## Integration and Test Execution Manual (EOP Server)

This section details the standardized procedures for initializing the local environment, establishing client-server communication, and thoroughly validating the Node Registry Protocol.

### 1. Environment Construction (Build Process)
Before proceeding with the execution, it is essential to compile the binaries using CMake. Make sure you are in the project root directory and run:

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
cd ..
```

---

### 2. Node Initialization and Connectivity

To validate the bidirectional ecosystem, concurrent instantiation of two isolated processes (Server and Client) is required. Instantiate each in a separate terminal or session:

#### Server Node (Terminal 1)
Deploys the main listener. The default operating port (9026) and idle timeout are specified in the `config.env` configuration file.

```bash
./build/eop_server
```
**Expected State:** The console should report that the service is active:
`Server started on port 9026. Inactivity timeout: 60s.`

#### Client Node (Terminal 2)
Set the network socket pointing to the local server's *host* and port:

```bash
./build/eop_client 127.0.0.1 9026
```
**Expected State:** After a successful connection, the interactive interpreter will enable keyboard input, notifying: `Connection established with 127.0.0.1:9026.`

---

### 3. Available Command Protocol (CLI Options)

Once the network layer is established on the client interface (Terminal 2), the user has a set of instructions for interacting with the server's persistence. Enter the following transactions to audit compliance with the data lifecycle:

#### A. Initial Registration and Welcome (REGISTER)
The initial session binding is completed atomically in the first registration transaction.
*   **Command:**
    ```text
    REGISTER node_id=alpha-01 bunker_name=Refugio-A ip_address=10.0.0.5 capacity=80
    ```
*   **Server Response:** Confirms registration and attaches the welcome *handshake* with the assigned identifier and timestamp:
    `ACK Server v1.0 | TIME <timestamp> | SESSION_ID alpha-01`
* **Audit (Terminal 1):** Internally records the adoption of an IP address and counts the increase in the concurrent session pool (`Total connections: 1`).

#### B. Duplicate Prevention (ERROR_DUPLICATE)
The system safeguards integrity by ensuring the strict uniqueness of the `node_id` field.

* **Command to inject:** (Attempt to reinject an existing ID)

    ```text
    REGISTER node_id=alpha-01 bunker_name=Zone-B ip_address=10.0.0.6 capacity=10
    ```
* **Server Response:** Blocks the overwrite and issues:

    `ERROR_DUPLICATE node_id=alpha-01 desc=Node already registered`

#### C. Specific Query (QUERY_NODE)
Allows you to extract the persisted attributes of an active node under its specific ID.

* **Command to Inject:**

    ```text
    QUERY_NODE node_id=alpha-01
    ```
* **Server Response:** `node_id=alpha-01 bunker_name=Refugio-A ip_address=10.0.0.5 capacity=80`

#### D. Global Query (LIST_NODES)
Iterates over the data structure to return all active linked elements.

* **Command to Inject:**

    ```text
    LIST_NODES
    ```
* **Server Response:** Emits a total count (`LIST_NODES count=X`), details all retrieved entries, and closes the transfer block using `END_LIST`.

#### E. Explicit Termination (EXIT/QUIT)
Connection closure managed directly by the client.

* **Command to Inject:**
    ```text
    exit
    ```
* **Server Response:** The server asynchronously closes the descriptor and logs the controlled decrease in the client quota (`Total connections: 0`).

---

### 4. Lifecycle: Idleness and Auditing (Timeouts & Logs)
* **Timeout (Idle Timeout):** The thread assigned to each socket is passively monitored. If the client does not transmit frames within 60 seconds (parameter `EOP_IDLE_TIMEOUT`), the connection expires and is released. The server terminal will validate the event by issuing an idle timeout.

* **Logging or Auditing System:** All critical events, edge resolutions, and syntax anomalies are formally and permanently indexed. Refer to the analytical file `system.log` mounted in the root directory during the test routine.
# System Architecture

This document describes the architecture of the system implemented by the team.

Recommended sections:

- System overview
- Components
- Data flow
- Deployment architecture

## Integration and Test Execution Manual (EOP Server)

This section details the standardized procedures for initializing the local environment, establishing client-server communication, and validating the Node Registry Protocol.

### 1. Environment Construction (Build Process)

Before proceeding with execution, compile the binaries using CMake. Make sure you are in the project root directory and run:

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
cd ..
```

---

### 2. Node Initialization and Connectivity

To validate the bidirectional ecosystem, concurrent instantiation of two isolated processes (Server and Client) is required. Instantiate each in a separate terminal or session.

#### Server Node (Terminal 1)

Deploys the main listener. The default operating port (9026) and idle timeout are specified in the `config.env` configuration file.

```bash
./build/eop_server
```

**Expected State:** The console should report that the service is active:
`Server started on port 9026. Inactivity timeout: 60s.`

#### Client Node (Terminal 2)

Set the network socket pointing to the local server host and port:

```bash
./build/eop_client 127.0.0.1 9026
```

**Expected State:** After a successful connection, the interactive interpreter will enable keyboard input, notifying:
`Connection established with 127.0.0.1:9026.`

---

### 3. Available Command Protocol (CLI Options)

Once the network layer is established on the client interface (Terminal 2), the user has a set of instructions for interacting with the server's persistence.

#### A. Initial Registration and Welcome (REGISTER)

The initial session binding is completed atomically in the first registration transaction.

- **Command:**

```text
REGISTER node_id=alpha-01 bunker_name=Refugio-A ip_address=10.0.0.5 capacity=80
```

- **Server Response:** Confirms registration and attaches the welcome handshake with the assigned identifier and timestamp:
  `ACK Server v1.0 | TIME <timestamp> | SESSION_ID alpha-01`
- **Audit (Terminal 1):** Internally records the adoption of an IP address and counts the increase in the concurrent session pool (`Total connections: 1`).

#### B. Duplicate Prevention (ERROR_DUPLICATE)

The system safeguards integrity by ensuring the strict uniqueness of the `node_id` field.

- **Command to inject:**

```text
REGISTER node_id=alpha-01 bunker_name=Zone-B ip_address=10.0.0.6 capacity=10
```

- **Server Response:** Blocks the overwrite and issues:
  `ERROR_DUPLICATE node_id=alpha-01 desc=Node already registered`

#### C. Specific Query (QUERY_NODE)

Allows you to extract the persisted attributes of an active node under its specific ID.

- **Command to inject:**

```text
QUERY_NODE node_id=alpha-01
```

- **Server Response:**
  `node_id=alpha-01 bunker_name=Refugio-A ip_address=10.0.0.5 capacity=80`

#### D. Global Query (LIST_NODES)

Iterates over the data structure to return all active linked elements.

- **Command to inject:**

```text
LIST_NODES
```

- **Server Response:** Emits a total count (`LIST_NODES count=X`), details all retrieved entries, and closes the transfer block using `END_LIST`.

#### E. Explicit Termination (EXIT/QUIT)

Connection closure managed directly by the client.

- **Command to inject:**

```text
exit
```

- **Server Response:** The server asynchronously closes the descriptor and logs the controlled decrease in the client quota (`Total connections: 0`).

---

### 4. Lifecycle: Idleness and Auditing (Timeouts & Logs)

- **Timeout (Idle Timeout):** The thread assigned to each socket is passively monitored. If the client does not transmit frames within 60 seconds (parameter `EOP_IDLE_TIMEOUT`), the connection expires and is released. The server terminal will validate the event by issuing an idle timeout.

- **Logging or Auditing System:** All critical events, edge resolutions, and syntax anomalies are formally and permanently indexed. Refer to the analytical file `system.log` mounted in the root directory during the test routine.

---

### 5. Performance Benchmarking (Latency Testing)

To audit the efficiency of the transport layer and ensure compliance with latency requirements under load, an automated benchmarking engine is available.

#### Running the Latency Benchmark

This procedure requires the server to be operating in a clean state (without any blocking connections).

1. **Environment Preparation (Terminal 1):**
   Ensure the binary is up to date and deploy the server:

```bash
./build/eop_server
```

2. **Running the Measurement Probe (Terminal 2):**
   From the project root, run the Python-based telemetry script. This script simulates transaction bursts to calculate round-trip time (RTT):

```bash
python3 tests/load/latency_check.py
```

**Reported Metrics:**

- **Min/Max Latency:** Limits detected in the server response.
- **Average RTT:** Average response time.
- **Packet Loss:** Integrity of received messages compared to sent messages.

---

### 6. File Descriptor Leak Validation

To validate descriptor stability under repeated client activity, a dedicated helper script is available. This validation is intended to support compliance with the file descriptor leak criterion under high connection churn.

#### Purpose

This script starts the server, measures the number of open file descriptors through `/proc/<pid>/fd`, executes a large number of client connection cycles, and compares the final descriptor count against the baseline.

Its purpose is to help detect potential file descriptor leaks during repeated:

- connect → disconnect cycles
- register → disconnect cycles

#### Running the FD Leak Validation

1. **Environment Preparation**
   Ensure the project is already built:

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
cd ..
```

2. **Run the Validation Script**
   From the project root, execute:

```bash
python3 tests/load/<fd_leak_script>.py 10000 register
python3 tests/load/<fd_leak_script>.py 10000 connect
```

**Parameters**

- **First argument:** number of cycles to execute
- **Second argument:**
  - `register` → connect, register a node, then disconnect
  - `connect` → connect and disconnect without registering

**Stronger Validation Run**

For a more aggressive stress validation:

```bash
python3 tests/load/<fd_leak_script>.py 1000000 register
```

**Expected Result**

The script reports:

- initial FD count
- final FD count
- FD delta

A successful run should end with:

```text
PASS: No significant file descriptor leak detected
```

If descriptor growth is above the tolerated threshold, the script will report:

```text
FAIL: Possible file descriptor leak detected
```
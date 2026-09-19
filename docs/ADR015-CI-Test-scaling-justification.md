# Load and Stability Testing Adjustments for CI

## Purpose and Justification for the Reduction

Originally, the system was designed to validate demanding technical scenarios defined in user stories to ensure server robustness:
- **US-102 AC8 (Load):** 1,000 concurrent clients making 10,000 requests each.

- **US-106 AC7 (Stability):** 1,000 sequential cycles of `send_command` → `response_free` to verify memory leaks.

However, during executions in Continuous Integration (CI) environments such as GitHub Actions, it was observed that the pipeline exceeded the recommended time limits. Full-scale testing caused the workflow to approach 15 minutes or experience timeouts, hindering the necessary agility in development.

For this reason, the parameters in `tests/unit/testLoad.cpp` and `tests/unit/testClient.cpp` have been adjusted for automated runs, reducing the absolute numbers but maintaining the main objective: to validate concurrency, proper descriptor management, and memory integrity under load.

## CI Test Strategy (Optimized)

The current CI parameters act as an integrity validation (smoke test) to confirm that the infrastructure remains operational:

### Load Test (Reduced from US-102 AC8)
- **20 concurrent clients**
- **50 requests per client**

### Stability Test (Reduced from US-106 AC7)
- **100 sequential cycles**

## Compliance Note

It is important to note that these adjustments **do not replace** the official requirements of the user stories. The original acceptance criteria (1,000 customers and 1,000 cycles) remain the system's design objectives.

This reduction is strictly an operational optimization to ensure that the CI flow is agile, resource-efficient, and effective during daily development. Final validation of the delivery must still be demonstrated against the original workloads defined in the project.

## Running the Original Workloads

The load test reads its size from the environment; CI sets nothing and keeps the reduced figures.

```bash
# US-102 AC8: 1,000 clients x 10,000 requests
EOP_LOAD_CLIENTS=1000 EOP_LOAD_REQUESTS_PER_CLIENT=10000 ./build/dev/unit_tests --gtest_filter='EopLoadTest.*'

# US-102 AC2: same per-client workload with 1 and with 100 clients (one request every 100 ms)
EOP_LOAD_CLIENTS=1   EOP_LOAD_REQUESTS_PER_CLIENT=300 EOP_LOAD_REQUEST_INTERVAL_MS=100 ./build/dev/unit_tests --gtest_filter='EopLoadTest.*'
EOP_LOAD_CLIENTS=100 EOP_LOAD_REQUESTS_PER_CLIENT=300 EOP_LOAD_REQUEST_INTERVAL_MS=100 ./build/dev/unit_tests --gtest_filter='EopLoadTest.*'

# US-102 AC4: 1M connect/disconnect cycles against a real server
python3 tests/evidence/fd_leak_cycles.py build/dev/eop_server
```

`EOP_LOAD_MAX_P99_MS=200` additionally asserts the P99 latency NFR. `tests/evidence/watchdog.py` wraps
any of them and stops the run before the machine runs out of memory or overheats.

Measured on 2026-09-19 (i7-8550U, 4 cores, plugged in): 10,000,000 requests from 1,000 clients in
445 s with 0 errors; P99 1.72 ms with 100 clients against 1.03 ms with 1 (x1.67, limit x2); P99
3.1 ms with 10 clients at saturation; and 1,020,000 connect/disconnect cycles with the server's
descriptor count unchanged (5 → 5).

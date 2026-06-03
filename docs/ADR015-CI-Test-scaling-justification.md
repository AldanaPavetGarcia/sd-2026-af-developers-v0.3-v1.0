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
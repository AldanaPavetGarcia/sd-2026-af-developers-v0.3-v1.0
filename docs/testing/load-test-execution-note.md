# Load Test Execution Note

## Purpose

This document clarifies how the high-load test is executed during development and CI validation.

The goal is to distinguish between:

- the **official acceptance criterion workload**
- the **reduced workload used for faster feedback in routine CI runs**

## Background

US-102 AC8 requires validation of the following scenario:

- **1000 clients**
- **10,000 requests per client**

This remains the official target workload for the feature.

During development, we observed that this full-scale test is expensive in GitHub Actions runtime and significantly increases CI duration and resource usage. Because of that, running the full workload on every iteration is not always practical for day-to-day validation.

## Current Testing Strategy

To reduce CI execution time and GitHub Actions consumption during regular development, a smaller load-test configuration may be used in fast validation runs.

Example reduced configuration:

- **20 clients**
- **50 requests per client**

This reduced configuration is used only as a **smoke/performance sanity check** to confirm that:

- the test setup still works
- the server still handles concurrent traffic correctly
- the load test infrastructure remains operational

## Important Clarification

The reduced load test **does not replace** the official requirement.

The acceptance criterion for US-102 AC8 is still based on the full-scale scenario:

- **1000 clients**
- **10,000 requests per client**

The reduced values are only intended to make iterative CI feedback faster and cheaper during active development.

## Validation Status

The full-scale load test has been executed separately and is the reference used to confirm the real requirement.

Therefore:

- the **requirement remains unchanged**
- the **reduced run is only an operational optimization for CI**
- final validation must still be associated with the original workload required by AC8

## Rationale

This approach provides a balance between:

- preserving the original acceptance criterion
- reducing CI turnaround time
- lowering GitHub Actions runtime consumption
- allowing faster iteration during pull request review

## Conclusion

The project may use a reduced load-test configuration for routine CI feedback, but this should always be understood as a **non-final validation mode**.

The official requirement for US-102 AC8 remains the full-scale load test, and compliance must be demonstrated against that original workload.
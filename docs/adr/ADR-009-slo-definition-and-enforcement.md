# ADR-009: Observability, SLO Definition, and Dashboard Enforcement

## Context and Problem

EOP is delivered with a contractual Service Level Agreement (SLA) mandating a $99.9\%$ monthly availability runtime. To verify compliance, the platform requires an internal observability stack capable of tracking performance indicators in real time. We must establish a clear framework for defining Service Level Indicators (SLIs), Service Level Objectives (SLOs), and automated Error Budgets.


## Decision

The platform will deploy a standardized observability stack using **Prometheus** for metrics collection and **Grafana** for real-time visualization and alerting. All SLO definitions will be maintained as infrastructure-as-code within the repository.

### 1. Formally Enforced Service Level Indicators (SLIs)

$$\text{Request Success Rate} = \frac{\text{Successful Responses}}{\text{Total Incoming Requests}} \times 100$$

$$\text{Latency Performance} = \text{Duration of transaction execution mapped at the } P99 \text{ percentile.}$$

### 2. Service Level Objectives (SLO Targets)

Compliance windows operate on a rolling evaluation mechanism:

| Objective Type | Target Threshold | Window Size | Monthly Error Budget Impact |
| --- | --- | --- | --- |
| **Service Availability** | $\ge 99.9\%$ | 30 Days Rolling | **43.8 Minutes** maximum allowed downtime |
| **C++ Core Server Latency** | $< 200 \text{ ms}$ ($P99$) | 5 Minutes Rolling | $0.1\%$ of total transactions |
| **HPC Engine Processing** | Evaluated against the benchmark targets defined for US-201 and US-206 | 5 Minutes Rolling | P99 latency must remain below $2\times$ the single-thread baseline defined by the benchmark report |

### 3. Operational Error Budget Allocation Policy

The actions taken by the engineering team are governed by the real-time status of the monthly Error Budget:

```
[Error Budget Status]
   ├── > 50% Remaining   --> Green Status: Normal operation, focus on feature delivery.
   ├── 10% - 50% Open     --> Yellow Status: Enhanced code review required, focus shifts to stability.
   └── < 10% Depleted    --> Red Status: Feature freeze enforced. 100% of engineering capacity shifts to reliability.
```

## Alternatives Considered

* **Ad-Hoc Log Scraping Routines:** Parsing text logs via regular expressions to count failures. This approach was rejected due to its high I/O disk overhead, lack of real-time visibility, and inability to handle metric collection at high scale.

* **Proprietary Monitoring Platforms:** Tools like Datadog or New Relic provide comprehensive out-of-the-box dashboards, but they introduce licensing costs and external network dependencies that are incompatible with isolated bunker environments.


## Consequences

* **Positive:** Provides concrete, empirical proof of platform stability. Performance metrics can be verified in real time during load testing scenarios.

* **Drawbacks:** Requires instrumenting the application code to expose metrics endpoints (e.g., Prometheus scraping targets) and managing the associated resource overhead.

* **Restrictions Introduced:**

* Manual dashboard configuration via the Grafana UI is strictly prohibited in production. All panel definitions must be stored as declarative JSON structures under `monitoring/grafana/dashboards/` and provisioned automatically during deployment.

* Maintaining a positive, non-depleted Error Budget is an absolute requirement for successful release verification and final project delivery.
# ADR-018: HPC Speedup Baseline Strategy

## Context and Problem

**US-206 AC1** requires a Grafana panel showing the HPC Engine's current parallel speedup ratio, and **US-206 AC4** requires an alert that fires when P99 processing latency exceeds 2x a "single-thread baseline" for more than 2 consecutive minutes. Both ACs assume the existence of a single-thread baseline latency to compare against — but nothing in the codebase or in ADR-010 (Parallelization Strategy) defines what that baseline *is*, where it comes from, or how it is kept valid as graph sizes vary.

This is not a cosmetic gap: `hpc::computeShortestPaths`, `computeConnectedComponents`, and `computeCentrality` all have processing times that scale with graph size (nodes and edges), so a single scalar baseline latency is only meaningful for requests whose graph size is close to the size at which the baseline was measured. Publishing `speedup = baseline / processing_time` for a request against a graph 100x larger or smaller than the baseline's reference size would produce a number that is arithmetically well-formed but operationally meaningless — worse than publishing nothing, because a wrong number looks like evidence.

Three sub-problems need a decision:

1. **Where does the baseline value come from** — measured, estimated, or computed live?
2. **How is a single scalar baseline made safe to compare against requests of varying graph size?**
3. **What happens before a real measurement exists** — the OpenMP Benchmark Report (US-202 AC5) that would produce a trustworthy single-thread number requires ≥8 physical cores to also validate the AC3 speedup target, which is not available on the development machine used at design time (see `hpc-engine/benchmarks/run_benchmark.sh` and the Fase 3 benchmark plan)?

## Decision

The baseline is a **configured, banded-size, measured** value — not computed live per request and not derived analytically:

1. **Source**: the single-thread (`OMP_NUM_THREADS=1`) P99 latency for a specific reference graph size, taken directly from the OpenMP Benchmark Report (US-202 AC5, `hpc-engine/benchmarks/BENCHMARK_REPORT.md`). This is real measured data, not an estimate.
2. **Configuration surface**: three environment-driven `ConfigMap` keys, read by `hpc-engine/src/metrics.cpp`'s `ComputeConfiguredSpeedupRatio`:
   - `EOP_HPC_BASELINE_P99_MS` — the measured single-thread P99 latency, in milliseconds.
   - `EOP_HPC_BASELINE_GRAPH_NODES` — the node count the baseline above was measured at.
   - `EOP_HPC_BASELINE_BAND_PERCENT` — how far a request's graph size may deviate from the reference size (as a percentage) and still be considered comparable. Defaults to 20% when unset.
3. **Banded comparison**: `ComputeConfiguredSpeedupRatio(requestGraphNodes, processingSeconds, outSpeedup)` returns a speedup value only when `requestGraphNodes` falls within `referenceNodes * (1 ± bandPercent/100)`. Outside that band, it returns `false` and no `speedup_ratio` value is published for that request — an empty panel for an out-of-band request, not a misleading one.
4. **Two consumers, one computation**:
   - **AC1**: `hpc_engine`'s `eop_hpc_speedup_ratio` gauge is set from this function inside `RecordHpcRequestMetric`, whenever a request's graph size is within band.
   - **AC4**: the Grafana alert `eop-hpc-p99-above-2x-baseline` (`monitoring/grafana/provisioning/alerting/eop-hpc-alerts.yaml`) compares the live P99 (`histogram_quantile(0.99, ...)`) against `eop_hpc_baseline_seconds * 2`, a gauge published once at startup from `EOP_HPC_BASELINE_P99_MS`.
5. **Configured, from a real measurement**: `k8s/configmap.yaml` sets `EOP_HPC_BASELINE_P99_MS=6744`, `EOP_HPC_BASELINE_GRAPH_NODES=10000`, `EOP_HPC_BASELINE_BAND_PERCENT=20`. The value comes from `hpc-engine/benchmarks/BENCHMARK_REPORT.md`'s 10,000-node, single-thread row: 6.024 s / 6.035 s (median) / 6.744 s across 3 runs — the worst of the three is used as a conservative P99 substitute, since the benchmark only records 3 repetitions per configuration rather than a full latency distribution. Before this ADR, `EOP_HPC_BASELINE_*` were commented-out placeholders because the machine available at design time had only 4 physical cores, which is not enough to also validate US-202 AC3's 8-thread target — but AC3 is a separate criterion from what this baseline needs: a *single-thread* measurement is valid regardless of physical core count, so it did not need to wait for 8-core hardware once the benchmark itself was fixed to test at the sizes and with the analysis US-202 actually specifies. Until then, `ComputeConfiguredSpeedupRatio` correctly returned `false` unconditionally (no baseline configured), so `eop_hpc_speedup_ratio` stayed unpublished and the P99 alert resolved to `NoDataState: NoData` rather than either firing falsely or displaying a number nobody measured — that fail-safe behavior remains in place for any request whose graph size now falls outside the configured 20% band.

## Alternatives Considered

| Alternative | Reason for Rejection |
| --- | --- |
| **Measure a live single-thread run alongside every parallel request** (run the same graph once with `OMP_NUM_THREADS=1` and once at the configured thread count, compare directly) | Doubles the cost of every `ANALYZE_GRAPH` request — including in production, where the single-thread run has no purpose other than measurement. Directly contradicts the NFR of processing analysis "in seconds" (US-206 release goal) by design. |
| **Derive an expected baseline analytically via Amdahl's Law** (compute the theoretical serial-fraction-adjusted single-thread time from the parallel run's own timing) | Produces a *predicted* number, not a *measured* one — the AC explicitly says "baseline", and Amdahl's Law is already used elsewhere in this project (US-202 AC5's benchmark analysis) as a way to *explain* a measured speedup, not to *manufacture* a substitute for measuring it. Circular: the theoretical model would be validated against the very data it's supposed to replace. |
| **Single scalar baseline with no size band** (compare every request's speedup against one fixed reference time regardless of graph size) | This is the shape of the problem this ADR exists to avoid — a fixed-size baseline compared against variable-size requests produces numbers that are increasingly wrong as request size diverges from the reference size, with no signal to the reader that the comparison has become invalid. |
| **Per-size-bucket baseline table** (measure and store a baseline at several graph sizes — e.g. 1K/10K/50K/100K — and interpolate) | More accurate across the full size range, but requires the Benchmark Report to produce single-thread measurements at every bucket, multiplies the configuration surface by the number of buckets, and is meaningfully more complex to provision and keep in sync than a single reference point with a tolerance band. Deferred as a possible v0.3+ refinement if the single-band approach proves too coarse in practice; not justified by evidence yet. |

## Justification (Trade-off Analysis)

* **Grounded in real data, not estimation**: both consumers ultimately trace back to one measured number from the Benchmark Report, satisfying the ACs' literal requirement of a "single-thread baseline" rather than a computed proxy for one.
* **Fails safe, not silent**: outside the configured band, or before the baseline is configured at all, the function returns `false` and both consumers (the gauge and the alert) simply have no data — an empty panel and a `NoDataState` alert are both visibly "not evaluated," never a wrong number presented as a real one.
* **Single source of truth for two consumers**: one function (`ComputeConfiguredSpeedupRatio`) and one set of configuration keys back both the AC1 gauge and the AC4 alert threshold, avoiding two independently-drifting definitions of "baseline."
* **No production overhead**: because the baseline is a static configured value, not a per-request live measurement, publishing it costs one comparison and one gauge write per request — no additional graph analysis work.

## Consequences

* **Positive**: `eop_hpc_speedup_ratio` and the P99-vs-baseline alert are both fully implemented, unit-tested (`hpc-engine/tests/test_metrics.cpp`), configured with a real measured baseline, and live-validated in Grafana's alerting engine. Re-wiring to a new baseline measurement (e.g. from different hardware) is a `ConfigMap` edit, not a code change.

* **Drawbacks**: a single reference size with a tolerance band still means requests outside the band publish no speedup data at all, which could read as "the feature isn't working" rather than "this request's graph size isn't comparable to the baseline" unless the dashboard documentation makes the banding behavior explicit (tracked as a documentation follow-up against `docs/hpc-performance-grafana-panel.md`).

* **Restrictions Introduced**:
  * `EOP_HPC_BASELINE_P99_MS` and `EOP_HPC_BASELINE_GRAPH_NODES` must always be set together, from the same Benchmark Report run — mixing a P99 value from one run with a reference node count from another would silently reintroduce exactly the mismatch this ADR exists to prevent.
  * Re-running the benchmark on different hardware (e.g. after moving from the 4-physical-core development machine to production-representative hardware) requires updating both values together and is a deployment-time configuration change, not a code change — this must be remembered as part of any future hardware migration, not just at initial setup.

## References

* `ADR-010-Parallelization-strategy-openmp.md` — establishes the OpenMP parallelization approach whose speedup this baseline measures.
* `ADR-012-Observability-slo-dashboard.md` — establishes the SLO/error-budget observability pattern this dashboard work extends into HPC-specific panels and alerts.
* `hpc-engine/benchmarks/run_benchmark.sh`, `hpc-engine/benchmarks/BENCHMARK_REPORT.md` — the source of the real measured baseline value (US-202 AC5).
* `hpc-engine/src/metrics.cpp` — `ComputeConfiguredSpeedupRatio`, `readPositiveDoubleEnv`, `InitHpcMetrics`/`ShutdownHpcMetrics` (`baseline_seconds` gauge).
* `k8s/configmap.yaml` — `EOP_HPC_BASELINE_P99_MS`, `EOP_HPC_BASELINE_GRAPH_NODES`, `EOP_HPC_BASELINE_BAND_PERCENT` (configured; see point 5 for where the value comes from).
* `monitoring/grafana/provisioning/alerting/eop-hpc-alerts.yaml` — the US-206 AC4 alert rule consuming `eop_hpc_baseline_seconds`.

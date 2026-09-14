#pragma once

/**
 * @file metrics.hpp
 * @brief Prometheus metrics instrumentation for the HPC Engine.
 */

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#ifndef EOP_HPC_METRICS_PORT
#define EOP_HPC_METRICS_PORT 9101
#endif

namespace hpc
{

struct HpcRequestMetrics
{
    std::string status = "ACK";
    double processing_seconds = 0.0;
    int thread_count = 0;
    int graph_nodes = 0;
    int graph_edges = 0;
    std::map<std::string, double> algorithm_durations_seconds;
};

struct HpcMetricsRegistry
{
    mutable std::mutex mutex;
    std::shared_ptr<prometheus::Registry> registry;
    std::unique_ptr<prometheus::Exposer> exposer;

    prometheus::Family<prometheus::Counter>* requests_total = nullptr;
    prometheus::Family<prometheus::Histogram>* processing_duration = nullptr;
    prometheus::Family<prometheus::Histogram>* algorithm_duration = nullptr;
    prometheus::Family<prometheus::Gauge>* thread_count = nullptr;
    prometheus::Family<prometheus::Gauge>* graph_nodes = nullptr;
    prometheus::Family<prometheus::Gauge>* graph_edges = nullptr;
    prometheus::Family<prometheus::Gauge>* speedup_ratio = nullptr;
    prometheus::Family<prometheus::Gauge>* baseline_seconds = nullptr;
    prometheus::Family<prometheus::Gauge>* active_threads = nullptr;
    prometheus::Family<prometheus::Counter>* thread_busy_seconds = nullptr;

    // Resolved once at init and read without holding `mutex`: ActiveThreadScope runs inside
    // OpenMP parallel regions, where taking the registry mutex would serialise every worker
    // against every other one. Gauge/Counter mutations are themselves atomic in
    // prometheus-cpp, so only the pointer handoff (init/shutdown) needs to be.
    std::atomic<prometheus::Gauge*> active_threads_child{nullptr};
    std::atomic<prometheus::Counter*> thread_busy_seconds_child{nullptr};
};

extern HpcMetricsRegistry hpcMetricsRegistry;

void InitHpcMetrics(bool start_exposer = true);
void ShutdownHpcMetrics();

void RecordHpcRequestMetric(const HpcRequestMetrics& metrics);
void RecordHpcErrorMetric(const std::string& status = "ERROR");

/**
 * Records speedup only when a real single-thread baseline exists.
 * Do not use thread_count as a proxy for speedup.
 */
void RecordHpcSpeedupRatio(double speedup);

/**
 * @brief RAII scope reporting one OpenMP worker as actively processing (US-206 AC6).
 *
 * @details Construct one instance per worker, as the first statement inside an OpenMP
 * parallel region -- never around it. Each live instance is one thread that is genuinely
 * executing algorithm work, which is what the acceptance criterion asks the panel to show:
 * how many of the configured OMP_NUM_THREADS are busy at any moment, as opposed to how many
 * were configured (already exposed as eop_hpc_thread_count).
 *
 * Two metrics come out of it, because an instantaneous gauge alone is misleading here: a
 * sub-second request is very likely to start and finish entirely between two Prometheus
 * scrapes, so the gauge would read 0 for a fully busy engine.
 * - eop_hpc_active_threads: instantaneous count, for the live "right now" reading.
 * - eop_hpc_thread_busy_seconds_total: accumulated thread-seconds, so the panel can chart
 *   rate(...[window]) -- the average number of threads busy over that window, immune to
 *   scrape timing. Utilisation is that rate divided by eop_hpc_thread_count.
 *
 * Cost is one atomic increment plus one clock read per worker per parallel region (not per
 * loop iteration), which is negligible against a region that runs for milliseconds or more.
 *
 * @pre Constructed inside an OpenMP parallel region.
 * @post On destruction the worker's elapsed time is added to the busy-seconds counter and
 *       the active count returns to its prior value.
 * @invariant The active count is >= 0, and returns to 0 once every region has been left.
 */
class ActiveThreadScope
{
  public:
    ActiveThreadScope();
    ~ActiveThreadScope();

    ActiveThreadScope(const ActiveThreadScope&) = delete;
    ActiveThreadScope& operator=(const ActiveThreadScope&) = delete;
    ActiveThreadScope(ActiveThreadScope&&) = delete;
    ActiveThreadScope& operator=(ActiveThreadScope&&) = delete;

  private:
    std::chrono::steady_clock::time_point m_start;
};

/**
 * @brief Number of workers currently inside an instrumented parallel region (US-206 AC6).
 * @return Live count; 0 when the engine is idle.
 */
int CurrentActiveThreadCount();

/**
 * @brief Computes eop_hpc_speedup_ratio (US-206 AC1) against a configured single-thread
 * baseline, when one is configured AND the request's graph size falls close enough to the
 * baseline's reference size to be a meaningful comparison. Called automatically from
 * RecordHpcRequestMetric() -- not part of the public per-request API surface, exposed here
 * only so tests can exercise the env-var parsing/banding logic directly.
 *
 * Configuration (all three must be present and valid, or nothing is recorded):
 * - EOP_HPC_BASELINE_P99_MS: single-thread P99 latency baseline, in milliseconds, measured for
 *   a specific graph size (comes from the OpenMP Benchmark Report, US-202 AC5).
 * - EOP_HPC_BASELINE_GRAPH_NODES: the node count the baseline above was measured at.
 * - EOP_HPC_BASELINE_BAND_PERCENT (default 20): a baseline measured at one graph size doesn't
 *   mean much compared to processing time for a very different size, so speedup is only
 *   published when requestGraphNodes falls within this percentage of the reference size.
 *
 * @param requestGraphNodes Node count of the graph just processed.
 * @param processingSeconds Wall-clock processing time for that request, in seconds.
 * @param outSpeedup Set to baseline_seconds / processingSeconds when applicable; untouched
 * otherwise.
 * @return true if a speedup value was computed (and outSpeedup was set), false otherwise.
 */
bool ComputeConfiguredSpeedupRatio(int requestGraphNodes, double processingSeconds, double& outSpeedup);

} // namespace hpc

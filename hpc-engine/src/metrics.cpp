#include "hpc/metrics.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace hpc
{

HpcMetricsRegistry hpcMetricsRegistry;

namespace
{
    const prometheus::Histogram::BucketBoundaries kProcessingBuckets = {
        0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10};

    constexpr double kDefaultBaselineBandPercent = 20.0;

    /**
     * @brief Reads a positive double from an env var. Returns std::nullopt if unset, empty, or
     * not a valid positive number -- never throws, degrades to "not configured" instead.
     */
    std::optional<double> readPositiveDoubleEnv(const char* name)
    {
        const char* raw = std::getenv(name);
        if (raw == nullptr || raw[0] == '\0')
        {
            return std::nullopt;
        }

        try
        {
            const double parsed = std::stod(raw);
            if (parsed > 0.0)
            {
                return parsed;
            }
        }
        catch (const std::exception&)
        {
            // Malformed value -- treated as "not configured" below.
        }

        return std::nullopt;
    }

    int getMetricsPort()
    {
        int metrics_port = EOP_HPC_METRICS_PORT;

        if (const char* env = std::getenv("EOP_HPC_METRICS_PORT"))
        {
            const int parsed = std::atoi(env);
            if (parsed > 0 && parsed <= 65535)
            {
                metrics_port = parsed;
            }
        }

        return metrics_port;
    }

    prometheus::Labels serviceLabels()
    {
        return {{"service", "hpc-engine"}};
    }
} // namespace

void InitHpcMetrics(bool start_exposer)
{
    std::lock_guard<std::mutex> lock(hpcMetricsRegistry.mutex);

    if (hpcMetricsRegistry.registry != nullptr)
    {
        return;
    }

    hpcMetricsRegistry.registry = std::make_shared<prometheus::Registry>();

    hpcMetricsRegistry.requests_total =
        &prometheus::BuildCounter()
             .Name("eop_hpc_requests_total")
             .Help("Total number of HPC graph analysis requests processed, labeled by status.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.processing_duration =
        &prometheus::BuildHistogram()
             .Name("eop_hpc_processing_duration_seconds")
             .Help("Total HPC graph analysis processing duration in seconds.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.algorithm_duration =
        &prometheus::BuildHistogram()
             .Name("eop_hpc_algorithm_duration_seconds")
             .Help("Per-algorithm HPC execution duration in seconds.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.thread_count =
        &prometheus::BuildGauge()
             .Name("eop_hpc_thread_count")
             .Help("OpenMP thread count observed by the HPC Engine.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.graph_nodes =
        &prometheus::BuildGauge()
             .Name("eop_hpc_graph_nodes")
             .Help("Number of graph nodes in the latest processed HPC request.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.graph_edges =
        &prometheus::BuildGauge()
             .Name("eop_hpc_graph_edges")
             .Help("Number of graph edges in the latest processed HPC request.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.speedup_ratio =
        &prometheus::BuildGauge()
             .Name("eop_hpc_speedup_ratio")
             .Help("HPC speedup ratio when a valid single-thread baseline is available.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.baseline_seconds =
        &prometheus::BuildGauge()
             .Name("eop_hpc_baseline_seconds")
             .Help("Configured single-thread processing time baseline (EOP_HPC_BASELINE_P99_MS), "
                   "in seconds. Absent until a real baseline is configured -- see US-206 AC1.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.active_threads =
        &prometheus::BuildGauge()
             .Name("eop_hpc_active_threads")
             .Help("OpenMP workers currently executing inside an algorithm parallel region "
                   "(US-206 AC6). Compare against eop_hpc_thread_count for utilisation.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.thread_busy_seconds =
        &prometheus::BuildCounter()
             .Name("eop_hpc_thread_busy_seconds_total")
             .Help("Accumulated thread-seconds spent inside algorithm parallel regions. "
                   "rate() over a window gives the average number of busy workers, which "
                   "unlike the instantaneous gauge does not depend on scrape timing.")
             .Register(*hpcMetricsRegistry.registry);

    hpcMetricsRegistry.active_threads_child.store(&hpcMetricsRegistry.active_threads->Add(serviceLabels()),
                                                  std::memory_order_release);
    hpcMetricsRegistry.thread_busy_seconds_child.store(
        &hpcMetricsRegistry.thread_busy_seconds->Add(serviceLabels()), std::memory_order_release);

    // Set once at startup, not per-request: the baseline is a fixed reference value from the
    // Benchmark Report (US-202 AC5), not something that changes while the engine is running.
    if (const auto baselineMs = readPositiveDoubleEnv("EOP_HPC_BASELINE_P99_MS"))
    {
        hpcMetricsRegistry.baseline_seconds->Add(serviceLabels()).Set(*baselineMs / 1000.0);
    }

    if (!start_exposer)
    {
        return;
    }

    try
    {
        const int metrics_port = getMetricsPort();
        hpcMetricsRegistry.exposer =
            std::make_unique<prometheus::Exposer>("0.0.0.0:" + std::to_string(metrics_port));
        hpcMetricsRegistry.exposer->RegisterCollectable(hpcMetricsRegistry.registry);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Failed to start HPC Prometheus /metrics exposer: " << ex.what() << '\n';
        hpcMetricsRegistry.exposer.reset();
    }
}

void ShutdownHpcMetrics()
{
    std::lock_guard<std::mutex> lock(hpcMetricsRegistry.mutex);

    hpcMetricsRegistry.exposer.reset();
    hpcMetricsRegistry.requests_total = nullptr;
    hpcMetricsRegistry.processing_duration = nullptr;
    hpcMetricsRegistry.algorithm_duration = nullptr;
    hpcMetricsRegistry.thread_count = nullptr;
    hpcMetricsRegistry.graph_nodes = nullptr;
    hpcMetricsRegistry.graph_edges = nullptr;
    hpcMetricsRegistry.speedup_ratio = nullptr;
    hpcMetricsRegistry.baseline_seconds = nullptr;
    hpcMetricsRegistry.active_threads = nullptr;
    hpcMetricsRegistry.thread_busy_seconds = nullptr;

    // Cleared before the registry drops the families they point into, so a scope that outlives
    // shutdown sees a null child and becomes a no-op instead of a dangling write.
    hpcMetricsRegistry.active_threads_child.store(nullptr, std::memory_order_release);
    hpcMetricsRegistry.thread_busy_seconds_child.store(nullptr, std::memory_order_release);

    hpcMetricsRegistry.registry.reset();
}

bool ComputeConfiguredSpeedupRatio(int requestGraphNodes, double processingSeconds, double& outSpeedup)
{
    const auto baselineMs = readPositiveDoubleEnv("EOP_HPC_BASELINE_P99_MS");
    const auto referenceNodes = readPositiveDoubleEnv("EOP_HPC_BASELINE_GRAPH_NODES");
    if (!baselineMs || !referenceNodes || processingSeconds <= 0.0)
    {
        return false;
    }

    const double bandPercent =
        readPositiveDoubleEnv("EOP_HPC_BASELINE_BAND_PERCENT").value_or(kDefaultBaselineBandPercent);
    const double lowerBound = *referenceNodes * (1.0 - bandPercent / 100.0);
    const double upperBound = *referenceNodes * (1.0 + bandPercent / 100.0);

    if (requestGraphNodes < lowerBound || requestGraphNodes > upperBound)
    {
        // Baseline measured at a meaningfully different graph size -- publishing a ratio here
        // would compare apples to oranges. Leaving the metric unpopulated for this request is
        // more honest than a misleading number.
        return false;
    }

    outSpeedup = (*baselineMs / 1000.0) / processingSeconds;
    return true;
}

namespace
{
    std::atomic<int> activeThreadCount{0};
} // namespace

ActiveThreadScope::ActiveThreadScope() : m_start(std::chrono::steady_clock::now())
{
    activeThreadCount.fetch_add(1, std::memory_order_relaxed);

    if (auto* gauge = hpcMetricsRegistry.active_threads_child.load(std::memory_order_acquire))
    {
        gauge->Increment();
    }
}

ActiveThreadScope::~ActiveThreadScope()
{
    const double busySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();

    if (auto* counter = hpcMetricsRegistry.thread_busy_seconds_child.load(std::memory_order_acquire))
    {
        counter->Increment(busySeconds);
    }

    if (auto* gauge = hpcMetricsRegistry.active_threads_child.load(std::memory_order_acquire))
    {
        gauge->Decrement();
    }

    activeThreadCount.fetch_sub(1, std::memory_order_relaxed);
}

int CurrentActiveThreadCount()
{
    return activeThreadCount.load(std::memory_order_relaxed);
}

void RecordHpcRequestMetric(const HpcRequestMetrics& metrics)
{
    std::lock_guard<std::mutex> lock(hpcMetricsRegistry.mutex);

    if (hpcMetricsRegistry.requests_total == nullptr || hpcMetricsRegistry.processing_duration == nullptr)
    {
        return;
    }

    hpcMetricsRegistry.requests_total->Add({{"service", "hpc-engine"}, {"status", metrics.status}}).Increment();

    if (metrics.status == "ACK")
    {
        hpcMetricsRegistry.processing_duration->Add(serviceLabels(), kProcessingBuckets)
            .Observe(metrics.processing_seconds);

        for (const auto& [algorithm, duration_seconds] : metrics.algorithm_durations_seconds)
        {
            hpcMetricsRegistry.algorithm_duration
                ->Add({{"service", "hpc-engine"}, {"algorithm", algorithm}}, kProcessingBuckets)
                .Observe(duration_seconds);
        }

        hpcMetricsRegistry.thread_count->Add(serviceLabels()).Set(metrics.thread_count);
        hpcMetricsRegistry.graph_nodes->Add(serviceLabels()).Set(metrics.graph_nodes);
        hpcMetricsRegistry.graph_edges->Add(serviceLabels()).Set(metrics.graph_edges);

        double speedup = 0.0;
        if (hpcMetricsRegistry.speedup_ratio != nullptr &&
            ComputeConfiguredSpeedupRatio(metrics.graph_nodes, metrics.processing_seconds, speedup))
        {
            hpcMetricsRegistry.speedup_ratio->Add(serviceLabels()).Set(speedup);
        }
    }
}

void RecordHpcErrorMetric(const std::string& status)
{
    HpcRequestMetrics metrics;
    metrics.status = status;
    RecordHpcRequestMetric(metrics);
}

void RecordHpcSpeedupRatio(double speedup)
{
    std::lock_guard<std::mutex> lock(hpcMetricsRegistry.mutex);

    if (hpcMetricsRegistry.speedup_ratio == nullptr)
    {
        return;
    }

    hpcMetricsRegistry.speedup_ratio->Add(serviceLabels()).Set(speedup);
}

} // namespace hpc

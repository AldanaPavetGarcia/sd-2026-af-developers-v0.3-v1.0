#include "hpc/metrics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace hpc
{

HpcMetricsRegistry hpcMetricsRegistry;

namespace
{
    const prometheus::Histogram::BucketBoundaries kProcessingBuckets = {
        0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10};

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
    hpcMetricsRegistry.registry.reset();
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

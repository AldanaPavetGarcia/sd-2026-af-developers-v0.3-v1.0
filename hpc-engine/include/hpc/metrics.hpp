#pragma once

/**
 * @file metrics.hpp
 * @brief Prometheus metrics instrumentation for the HPC Engine.
 */

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

} // namespace hpc

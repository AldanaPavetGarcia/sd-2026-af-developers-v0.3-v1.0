#pragma once

/**
 * @file tracing.hpp
 * @brief OpenTelemetry tracing instrumentation for the HPC Engine.
 *
 * @details The engine receives a W3C `traceparent` in every ANALYZE_GRAPH envelope
 * (docs/analyze-graph-protocol-v0.2.md). Until now it only echoed that value back in the
 * response, so engine-side work was invisible in the trace: US-203 AC5 / US-206 AC5 ask for the
 * whole client -> dispatch -> engine chain under one trace, and the engine contributed no span
 * of its own. These helpers let it emit spans that attach to the caller's trace as children.
 */

#include <memory>
#include <string>

#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/span.h>

namespace hpc
{

    /**
     * @brief Initializes the engine's tracer provider and OTLP exporter.
     *
     * @details Reads `OTEL_EXPORTER_OTLP_ENDPOINT` for the collector URL, falling back to the same
     * localhost default eop-server uses. Setting `EOP_TRACING_ENABLED=0` disables tracing entirely,
     * matching eop-server's switch, so batch-mode runs and tests can opt out. Safe to call once at
     * startup; calling it again without an intervening shutdown has no additional effect.
     *
     * @post Spans created by StartAnalyzeGraphSpan() are exported until ShutdownHpcTracer() runs.
     */
    void InitHpcTracer();

    /**
     * @brief Flushes pending spans and tears down the tracer provider.
     *
     * @details Must run before process exit, otherwise spans still sitting in the batch processor
     * are lost — which is exactly what makes short-lived batch-mode runs appear to emit nothing.
     */
    void ShutdownHpcTracer();

    /**
     * @brief Starts a span for one ANALYZE_GRAPH request, parented to the caller's trace.
     *
     * @param traceparent W3C trace context from the request envelope. When empty or malformed, the
     *                    span still starts but as the root of a new trace, so engine work is never
     *                    dropped just because the caller omitted the field.
     * @param request_id  Request identifier, attached as a span attribute for correlation.
     * @param graph_nodes Node count of the graph being analyzed, attached as an attribute.
     * @param graph_edges Edge count of the graph being analyzed, attached as an attribute.
     * @return The started span. Never null; ending it is the caller's responsibility.
     */
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> StartAnalyzeGraphSpan(const std::string& traceparent,
                                                                                       const std::string& request_id,
                                                                                       int graph_nodes,
                                                                                       int graph_edges);

    /**
     * @brief Starts a child span covering a single algorithm's execution.
     *
     * @param parent    Span returned by StartAnalyzeGraphSpan(); the algorithm span attaches to it.
     * @param algorithm Algorithm identifier (`shortest_paths`, `connected_components`, `centrality`).
     * @return The started span. Never null; ending it is the caller's responsibility.
     */
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>
    StartAlgorithmSpan(const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>& parent,
                       const std::string& algorithm);

} // namespace hpc

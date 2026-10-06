#include "hpc/engine.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include <omp.h>

#include "hpc/centrality.hpp"
#include "hpc/connected_components.hpp"
#include "hpc/dijkstra.hpp"
#include "hpc/json_parser.hpp"
#include "hpc/metrics.hpp"
#include "hpc/protocol.hpp"
#include "hpc/tracing.hpp"
#include "hpc/validator.hpp"

namespace hpc
{

    namespace
    {
        // processAnalyzeGraphRequest is a module boundary (called across the hpc-engine /
        // main.cpp socket-and-batch dispatch): per project convention, exceptions must not
        // cross it. error_handler_t::replace makes dump() itself unable to throw
        // type_error.316 on invalid UTF-8, instead of trusting every call site to be safe.
        constexpr auto DUMP_ERROR_HANDLER = nlohmann::json::error_handler_t::replace;

        using Clock = std::chrono::high_resolution_clock;

        double elapsedMilliseconds(const Clock::time_point& start, const Clock::time_point& end)
        {
            return std::chrono::duration<double, std::milli>(end - start).count();
        }

        // Position of the trace-id inside a W3C traceparent ("00-<trace-id>-<span-id>-<flags>").
        constexpr std::size_t TRACEPARENT_TRACE_ID_OFFSET = 3;
        constexpr std::size_t TRACEPARENT_TRACE_ID_LENGTH = 32;

        std::string traceIdOf(const std::string& traceparent)
        {
            if (traceparent.size() < TRACEPARENT_TRACE_ID_OFFSET + TRACEPARENT_TRACE_ID_LENGTH)
            {
                return "";
            }

            return traceparent.substr(TRACEPARENT_TRACE_ID_OFFSET, TRACEPARENT_TRACE_ID_LENGTH);
        }

        std::string utcTimestamp()
        {
            const auto now = std::chrono::system_clock::now();
            const auto seconds = std::chrono::system_clock::to_time_t(now);
            const auto millis =
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

            std::tm utc {};
            gmtime_r(&seconds, &utc);

            std::ostringstream out;
            out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << millis
                << 'Z';
            return out.str();
        }

        // US-201 AC5: every request is logged with the graph size received, the algorithms
        // executed, the wall-clock processing time and the thread count. Written as one JSON
        // object per line on stderr, matching the structured-log shape the rest of the engine
        // already emits (see logRetryEvent in main.cpp) so a log collector can parse it without
        // a per-service format. stdout is reserved for protocol responses in batch mode, which
        // is why this cannot go there.
        void logRequestSummary(const std::string& request_id,
                               const std::string& traceparent,
                               int graph_nodes,
                               int graph_edges,
                               const nlohmann::json& executed_algorithms,
                               double processing_ms,
                               int thread_count)
        {
            nlohmann::json entry;
            entry["timestamp"] = utcTimestamp();
            entry["level"] = "info";
            entry["service"] = "hpc-engine";
            entry["event"] = "analyze_graph_completed";
            entry["request_id"] = request_id;
            entry["graph_nodes"] = graph_nodes;
            entry["graph_edges"] = graph_edges;
            entry["algorithms"] = executed_algorithms;
            entry["processing_ms"] = processing_ms;
            entry["thread_count"] = thread_count;

            const std::string trace_id = traceIdOf(traceparent);
            if (!trace_id.empty())
            {
                entry["trace_id"] = trace_id;
            }

            std::cerr << entry.dump(-1, ' ', false, DUMP_ERROR_HANDLER) << '\n';
        }
    } // namespace

    EngineProcessResult processAnalyzeGraphRequest(const std::string& raw_input)
    {
        try
        {
            if (raw_input.empty())
            {
                auto err = hpc::buildErrorResponse("", "ERROR_INVALID_REQUEST", "Empty input received.", "", false);
                hpc::RecordHpcErrorMetric("ERROR");
                return {err.dump(-1, ' ', false, DUMP_ERROR_HANDLER) + "\n", 1};
            }

            auto parse_result = hpc::parseRequest(raw_input);
            if (!parse_result.ok)
            {
                auto err = hpc::buildErrorResponse(parse_result.request.request_id,
                                                   parse_result.error_code,
                                                   parse_result.error_message,
                                                   parse_result.error_field,
                                                   false);
                hpc::RecordHpcErrorMetric("ERROR");
                return {err.dump(-1, ' ', false, DUMP_ERROR_HANDLER) + "\n", 1};
            }

            auto& req = parse_result.request;

            auto validation = hpc::validateGraph(req.graph, req.algorithms);
            if (!validation.ok)
            {
                auto err = hpc::buildErrorResponse(
                    req.request_id, validation.error_code, validation.error_message, validation.error_field, false);
                hpc::RecordHpcErrorMetric("ERROR");
                return {err.dump(-1, ' ', false, DUMP_ERROR_HANDLER) + "\n", 1};
            }

            const int thread_count = omp_get_max_threads();

            // Attaches to the caller's trace via the envelope's traceparent, so engine work shows
            // up as a child of the request that caused it (US-203 AC5 / US-206 AC5).
            auto requestSpan =
                hpc::StartAnalyzeGraphSpan(req.traceparent, req.request_id, req.graph.num_nodes, req.graph.num_edges);
            requestSpan->SetAttribute("thread.count", thread_count);

            const auto processing_start = Clock::now();

            nlohmann::json algorithm_results = nlohmann::json::object();
            nlohmann::json executed_algorithms = nlohmann::json::array();
            nlohmann::json algorithm_timings_ms = nlohmann::json::object();
            nlohmann::json algorithm_metadata = nlohmann::json::object();
            std::map<std::string, double> algorithm_durations_seconds;

            for (const auto& algo : req.algorithms)
            {
                auto algorithmSpan = hpc::StartAlgorithmSpan(requestSpan, algo);
                const auto algorithm_start = Clock::now();

                if (algo == "shortest_paths")
                {
                    algorithm_results["shortest_paths"] = hpc::computeShortestPaths(req.graph);
                }
                else if (algo == "connected_components")
                {
                    algorithm_results["connected_components"] = hpc::computeConnectedComponents(req.graph);
                }
                else if (algo == "centrality")
                {
                    hpc::CentralityStats centrality_stats;
                    algorithm_results["centrality"] = hpc::computeCentrality(req.graph, centrality_stats);

                    // Above the exact-computation threshold the scores are an estimate. Saying so
                    // in the response is what keeps a caller from reading them as exact figures
                    // (ADR-024).
                    if (centrality_stats.approximate)
                    {
                        algorithm_metadata["centrality"] = {
                            {"approximate", true},
                            {"method", "brandes_pivot_sampling"},
                            {"sampled_sources", centrality_stats.sampled_sources},
                            {"total_sources", centrality_stats.total_sources},
                        };
                        algorithmSpan->SetAttribute("centrality.approximate", true);
                        algorithmSpan->SetAttribute("centrality.sampled_sources", centrality_stats.sampled_sources);
                    }
                }

                const auto algorithm_end = Clock::now();
                const double algorithm_ms = elapsedMilliseconds(algorithm_start, algorithm_end);
                executed_algorithms.push_back(algo);
                algorithm_timings_ms[algo] = algorithm_ms;
                algorithm_durations_seconds[algo] = algorithm_ms / 1000.0;

                algorithmSpan->SetAttribute("duration.ms", algorithm_ms);
                algorithmSpan->End();
            }

            const auto processing_end = Clock::now();
            const double processing_ms = elapsedMilliseconds(processing_start, processing_end);

            requestSpan->SetAttribute("processing.ms", processing_ms);
            requestSpan->SetAttribute("result", "ACK");
            requestSpan->End();

            auto response = hpc::buildAckResponse(req.request_id,
                                                  req.traceparent,
                                                  req.graph.num_nodes,
                                                  req.graph.num_edges,
                                                  processing_ms,
                                                  thread_count,
                                                  algorithm_results,
                                                  executed_algorithms,
                                                  algorithm_timings_ms,
                                                  algorithm_metadata);

            HpcRequestMetrics metrics;
            metrics.status = "ACK";
            metrics.processing_seconds = processing_ms / 1000.0;
            metrics.thread_count = thread_count;
            metrics.graph_nodes = req.graph.num_nodes;
            metrics.graph_edges = req.graph.num_edges;
            metrics.algorithm_durations_seconds = algorithm_durations_seconds;
            hpc::RecordHpcRequestMetric(metrics);

            logRequestSummary(req.request_id,
                              req.traceparent,
                              req.graph.num_nodes,
                              req.graph.num_edges,
                              executed_algorithms,
                              processing_ms,
                              thread_count);

            return {response.dump(-1, ' ', false, DUMP_ERROR_HANDLER) + "\n", 0};
        }
        catch (const std::exception& e)
        {
            auto err = hpc::buildErrorResponse(
                "", "ERROR_ANALYSIS_FAILED", std::string("Internal engine error: ") + e.what(), "", true);
            hpc::RecordHpcErrorMetric("ERROR");
            return {err.dump(-1, ' ', false, DUMP_ERROR_HANDLER) + "\n", 2};
        }
        catch (...)
        {
            // Module boundary: must not let a non-std::exception-derived throw escape into
            // main.cpp's socket-mode request loop (would abort the process, see main.cpp's
            // HeartbeatGuard doc comment for the concrete crash path this closes).
            auto err = hpc::buildErrorResponse(
                "", "ERROR_ANALYSIS_FAILED", "Internal engine error: unknown exception", "", true);
            hpc::RecordHpcErrorMetric("ERROR");
            return {err.dump(-1, ' ', false, DUMP_ERROR_HANDLER) + "\n", 2};
        }
    }

} // namespace hpc

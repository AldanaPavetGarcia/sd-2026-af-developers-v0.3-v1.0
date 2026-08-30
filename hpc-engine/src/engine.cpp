#include "hpc/engine.hpp"

#include <chrono>
#include <map>
#include <string>

#include <omp.h>

#include "hpc/centrality.hpp"
#include "hpc/connected_components.hpp"
#include "hpc/dijkstra.hpp"
#include "hpc/json_parser.hpp"
#include "hpc/metrics.hpp"
#include "hpc/protocol.hpp"
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
            const auto processing_start = Clock::now();

            nlohmann::json algorithm_results = nlohmann::json::object();
            nlohmann::json executed_algorithms = nlohmann::json::array();
            nlohmann::json algorithm_timings_ms = nlohmann::json::object();
            std::map<std::string, double> algorithm_durations_seconds;

            for (const auto& algo : req.algorithms)
            {
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
                    algorithm_results["centrality"] = hpc::computeCentrality(req.graph);
                }

                const auto algorithm_end = Clock::now();
                const double algorithm_ms = elapsedMilliseconds(algorithm_start, algorithm_end);
                executed_algorithms.push_back(algo);
                algorithm_timings_ms[algo] = algorithm_ms;
                algorithm_durations_seconds[algo] = algorithm_ms / 1000.0;
            }

            const auto processing_end = Clock::now();
            const double processing_ms = elapsedMilliseconds(processing_start, processing_end);

            auto response = hpc::buildAckResponse(req.request_id,
                                                  req.traceparent,
                                                  req.graph.num_nodes,
                                                  req.graph.num_edges,
                                                  processing_ms,
                                                  thread_count,
                                                  algorithm_results,
                                                  executed_algorithms,
                                                  algorithm_timings_ms);

            HpcRequestMetrics metrics;
            metrics.status = "ACK";
            metrics.processing_seconds = processing_ms / 1000.0;
            metrics.thread_count = thread_count;
            metrics.graph_nodes = req.graph.num_nodes;
            metrics.graph_edges = req.graph.num_edges;
            metrics.algorithm_durations_seconds = algorithm_durations_seconds;
            hpc::RecordHpcRequestMetric(metrics);

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

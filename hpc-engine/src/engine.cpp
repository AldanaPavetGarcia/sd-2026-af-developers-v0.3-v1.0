#include "hpc/engine.hpp"

#include <chrono>
#include <string>

#include <omp.h>

#include "hpc/centrality.hpp"
#include "hpc/connected_components.hpp"
#include "hpc/dijkstra.hpp"
#include "hpc/json_parser.hpp"
#include "hpc/protocol.hpp"
#include "hpc/validator.hpp"

namespace hpc
{

    EngineProcessResult processAnalyzeGraphRequest(const std::string& raw_input)
    {
        try
        {
            if (raw_input.empty())
            {
                auto err = hpc::buildErrorResponse("", "ERROR_INVALID_REQUEST", "Empty input received.", "", false);
                return {err.dump() + "\n", 1};
            }

            auto parse_result = hpc::parseRequest(raw_input);
            if (!parse_result.ok)
            {
                auto err = hpc::buildErrorResponse(parse_result.request.request_id,
                                                   parse_result.error_code,
                                                   parse_result.error_message,
                                                   parse_result.error_field,
                                                   false);
                return {err.dump() + "\n", 1};
            }

            auto& req = parse_result.request;

            auto validation = hpc::validateGraph(req.graph, req.algorithms);
            if (!validation.ok)
            {
                auto err = hpc::buildErrorResponse(
                    req.request_id, validation.error_code, validation.error_message, validation.error_field, false);
                return {err.dump() + "\n", 1};
            }

            const int thread_count = omp_get_max_threads();
            const auto start = std::chrono::high_resolution_clock::now();

            nlohmann::json algorithm_results = nlohmann::json::object();

            for (const auto& algo : req.algorithms)
            {
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
            }

            const auto end = std::chrono::high_resolution_clock::now();
            const double processing_ms = std::chrono::duration<double, std::milli>(end - start).count();

            auto response = hpc::buildAckResponse(req.request_id,
                                                  req.traceparent,
                                                  req.graph.num_nodes,
                                                  req.graph.num_edges,
                                                  processing_ms,
                                                  thread_count,
                                                  algorithm_results);

            return {response.dump() + "\n", 0};
        }
        catch (const std::exception& e)
        {
            auto err = hpc::buildErrorResponse(
                "", "ERROR_ANALYSIS_FAILED", std::string("Internal engine error: ") + e.what(), "", true);
            return {err.dump() + "\n", 2};
        }
    }

} // namespace hpc

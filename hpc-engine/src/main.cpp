/**
 * @file main.cpp
 * @brief HPC Engine entry point: reads JSON from stdin, processes graph, writes result to stdout.
 *
 * Exit codes:
 *   0 — Success (ACK response written to stdout)
 *   1 — Validation or parse error (ERROR response written to stdout)
 *   2 — Unexpected internal error
 */

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

#include <omp.h>

#include "hpc/centrality.hpp"
#include "hpc/connected_components.hpp"
#include "hpc/dijkstra.hpp"
#include "hpc/json_parser.hpp"
#include "hpc/protocol.hpp"
#include "hpc/validator.hpp"

int main()
{
    try
    {
        // Read entire stdin into a string
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        std::string raw_input = buffer.str();

        if (raw_input.empty())
        {
            auto err = hpc::buildErrorResponse(
                "", "ERROR_INVALID_REQUEST",
                "Empty input received.", "", false);
            std::cout << err.dump() << std::endl;
            return 1;
        }

        // Parse JSON request
        auto parse_result = hpc::parseRequest(raw_input);
        if (!parse_result.ok)
        {
            auto err = hpc::buildErrorResponse(
                parse_result.request.request_id,
                parse_result.error_code,
                parse_result.error_message,
                parse_result.error_field,
                false);
            std::cout << err.dump() << std::endl;
            return 1;
        }

        auto& req = parse_result.request;

        // Validate graph
        auto validation = hpc::validateGraph(req.graph, req.algorithms);
        if (!validation.ok)
        {
            auto err = hpc::buildErrorResponse(
                req.request_id,
                validation.error_code,
                validation.error_message,
                validation.error_field,
                false);
            std::cout << err.dump() << std::endl;
            return 1;
        }

        // Record thread count and start timer
        int thread_count = omp_get_max_threads();
        auto start = std::chrono::high_resolution_clock::now();

        // Execute requested algorithms
        nlohmann::json algorithm_results = nlohmann::json::object();

        for (const auto& algo : req.algorithms)
        {
            if (algo == "shortest_paths")
            {
                algorithm_results["shortest_paths"] =
                    hpc::computeShortestPaths(req.graph);
            }
            else if (algo == "connected_components")
            {
                algorithm_results["connected_components"] =
                    hpc::computeConnectedComponents(req.graph);
            }
            else if (algo == "centrality")
            {
                algorithm_results["centrality"] =
                    hpc::computeCentrality(req.graph);
            }
        }

        // Stop timer
        auto end = std::chrono::high_resolution_clock::now();
        double processing_ms =
            std::chrono::duration<double, std::milli>(end - start).count();

        // Build and output ACK response
        auto response = hpc::buildAckResponse(
            req.request_id,
            req.traceparent,
            req.graph.num_nodes,
            req.graph.num_edges,
            processing_ms,
            thread_count,
            algorithm_results);

        std::cout << response.dump() << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        auto err = hpc::buildErrorResponse(
            "", "ERROR_ANALYSIS_FAILED",
            std::string("Internal engine error: ") + e.what(),
            "", true);
        std::cout << err.dump() << std::endl;
        return 2;
    }
}

#ifndef HPC_PROTOCOL_HPP
#define HPC_PROTOCOL_HPP

/**
 * @file protocol.hpp
 * @brief JSON response builders for the ANALYZE_GRAPH v0.2 protocol.
 */

#include <nlohmann/json.hpp>
#include <string>

namespace hpc
{

/**
 * @brief Build a successful ACK response.
 *
 * @param request_id             Original request identifier.
 * @param traceparent            Trace context (may be empty).
 * @param graph_nodes            Number of nodes processed.
 * @param graph_edges            Number of edges processed.
 * @param processing_ms          Total wall-clock processing time in milliseconds.
 * @param thread_count           Number of OpenMP threads available to the engine.
 * @param algorithm_results      Map of algorithm name -> JSON result.
 * @param executed_algorithms    Ordered list of algorithm identifiers executed.
 * @param algorithm_timings_ms   Map of algorithm name -> wall-clock duration in milliseconds.
 * @return Complete JSON response object.
 */
nlohmann::json buildAckResponse(
    const std::string& request_id,
    const std::string& traceparent,
    int graph_nodes,
    int graph_edges,
    double processing_ms,
    int thread_count,
    const nlohmann::json& algorithm_results,
    const nlohmann::json& executed_algorithms,
    const nlohmann::json& algorithm_timings_ms);

/**
 * @brief Build a structured ERROR response.
 *
 * @param request_id    Original request identifier (may be empty).
 * @param error_code    Machine-readable error code.
 * @param message       Human-readable error description.
 * @param field         Field that caused the error (optional).
 * @param retryable     Whether the request can be retried.
 * @return Complete JSON error response object.
 */
nlohmann::json buildErrorResponse(
    const std::string& request_id,
    const std::string& error_code,
    const std::string& message,
    const std::string& field = "",
    bool retryable = false);

} // namespace hpc

#endif // HPC_PROTOCOL_HPP

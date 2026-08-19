/**
 * @file protocol.cpp
 * @brief JSON response builders for the ANALYZE_GRAPH v0.2 protocol.
 */

#include "hpc/protocol.hpp"

namespace hpc
{

nlohmann::json buildAckResponse(
    const std::string& request_id,
    const std::string& traceparent,
    int graph_nodes,
    int graph_edges,
    double processing_ms,
    int thread_count,
    const nlohmann::json& algorithm_results,
    const nlohmann::json& executed_algorithms,
    const nlohmann::json& algorithm_timings_ms)
{
    nlohmann::json response;
    response["type"] = "RESPONSE";
    response["status"] = "ACK";
    response["request_id"] = request_id;

    if (!traceparent.empty())
    {
        response["traceparent"] = traceparent;
    }

    response["result"]["graph_size"]["nodes"] = graph_nodes;
    response["result"]["graph_size"]["edges"] = graph_edges;
    response["result"]["processing_time_ms"] = processing_ms;
    response["result"]["thread_count"] = thread_count;
    response["result"]["algorithms"] = algorithm_results;
    response["result"]["executed_algorithms"] = executed_algorithms;
    response["result"]["algorithm_timings_ms"] = algorithm_timings_ms;

    return response;
}

nlohmann::json buildErrorResponse(
    const std::string& request_id,
    const std::string& error_code,
    const std::string& message,
    const std::string& field,
    bool retryable)
{
    nlohmann::json response;
    response["type"] = "RESPONSE";
    response["status"] = "ERROR";

    if (!request_id.empty())
    {
        response["request_id"] = request_id;
    }

    response["error_code"] = error_code;
    response["message"] = message;

    if (!field.empty())
    {
        response["field"] = field;
    }

    response["retryable"] = retryable;

    return response;
}

} // namespace hpc

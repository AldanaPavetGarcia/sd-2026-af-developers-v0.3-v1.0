/**
 * @file json_parser.cpp
 * @brief Implementation of JSON ingestion layer for ANALYZE_GRAPH requests.
 */

#include "hpc/json_parser.hpp"

#include <cmath>

namespace hpc
{

ParseResult parseRequest(const std::string& raw_json)
{
    ParseResult result;

    // Attempt to parse JSON
    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(raw_json);
    }
    catch (const nlohmann::json::parse_error& e)
    {
        result.ok = false;
        result.error_code = "ERROR_INVALID_REQUEST";
        result.error_message = std::string("Invalid JSON: ") + e.what();
        return result;
    }

    // Validate top-level structure
    if (!doc.contains("type") || doc["type"] != "ANALYZE_GRAPH")
    {
        result.ok = false;
        result.error_code = "ERROR_INVALID_REQUEST";
        result.error_message = "Missing or invalid 'type' field. Expected 'ANALYZE_GRAPH'.";
        result.error_field = "type";
        return result;
    }

    if (!doc.contains("request_id") || !doc["request_id"].is_string())
    {
        result.ok = false;
        result.error_code = "ERROR_INVALID_REQUEST";
        result.error_message = "Missing or invalid 'request_id' field.";
        result.error_field = "request_id";
        return result;
    }

    result.request.request_id = doc["request_id"].get<std::string>();

    // Optional trace context
    if (doc.contains("traceparent") && doc["traceparent"].is_string())
    {
        result.request.traceparent = doc["traceparent"].get<std::string>();
    }
    if (doc.contains("tracestate") && doc["tracestate"].is_string())
    {
        result.request.tracestate = doc["tracestate"].get<std::string>();
    }

    // Payload
    if (!doc.contains("payload") || !doc["payload"].is_object())
    {
        result.ok = false;
        result.error_code = "ERROR_INVALID_REQUEST";
        result.error_message = "Missing or invalid 'payload' field.";
        result.error_field = "payload";
        return result;
    }

    const auto& payload = doc["payload"];

    // Nodes
    if (!payload.contains("nodes") || !payload["nodes"].is_array())
    {
        result.ok = false;
        result.error_code = "ERROR_INVALID_REQUEST";
        result.error_message = "Missing or invalid 'payload.nodes' field.";
        result.error_field = "payload.nodes";
        return result;
    }

    Graph& graph = result.request.graph;
    const auto& nodes_arr = payload["nodes"];

    graph.num_nodes = static_cast<int>(nodes_arr.size());
    graph.node_names.reserve(graph.num_nodes);
    graph.adjacency.resize(graph.num_nodes);

    for (int i = 0; i < graph.num_nodes; ++i)
    {
        if (!nodes_arr[i].is_string())
        {
            result.ok = false;
            result.error_code = "ERROR_INVALID_REQUEST";
            result.error_message = "Node identifier must be a string.";
            result.error_field = "payload.nodes[" + std::to_string(i) + "]";
            return result;
        }
        std::string name = nodes_arr[i].get<std::string>();
        graph.node_names.push_back(name);
        graph.node_index[name] = i;
    }

    // Edges
    if (!payload.contains("edges") || !payload["edges"].is_array())
    {
        result.ok = false;
        result.error_code = "ERROR_INVALID_REQUEST";
        result.error_message = "Missing or invalid 'payload.edges' field.";
        result.error_field = "payload.edges";
        return result;
    }

    const auto& edges_arr = payload["edges"];
    graph.num_edges = static_cast<int>(edges_arr.size());

    for (int i = 0; i < graph.num_edges; ++i)
    {
        const auto& edge_obj = edges_arr[i];

        if (!edge_obj.is_object())
        {
            result.ok = false;
            result.error_code = "ERROR_INVALID_REQUEST";
            result.error_message = "Edge must be an object.";
            result.error_field = "payload.edges[" + std::to_string(i) + "]";
            return result;
        }

        if (!edge_obj.contains("from") || !edge_obj["from"].is_string())
        {
            result.ok = false;
            result.error_code = "ERROR_INVALID_REQUEST";
            result.error_message = "Edge missing or invalid 'from' field.";
            result.error_field = "payload.edges[" + std::to_string(i) + "].from";
            return result;
        }

        if (!edge_obj.contains("to") || !edge_obj["to"].is_string())
        {
            result.ok = false;
            result.error_code = "ERROR_INVALID_REQUEST";
            result.error_message = "Edge missing or invalid 'to' field.";
            result.error_field = "payload.edges[" + std::to_string(i) + "].to";
            return result;
        }

        if (!edge_obj.contains("weight") || !edge_obj["weight"].is_number())
        {
            result.ok = false;
            result.error_code = "ERROR_INVALID_REQUEST";
            result.error_message = "Edge missing or invalid 'weight' field.";
            result.error_field = "payload.edges[" + std::to_string(i) + "].weight";
            return result;
        }

        std::string from_name = edge_obj["from"].get<std::string>();
        std::string to_name = edge_obj["to"].get<std::string>();
        double weight = edge_obj["weight"].get<double>();

        // Look up node indices (structural mapping; validation happens later)
        auto from_it = graph.node_index.find(from_name);
        auto to_it = graph.node_index.find(to_name);

        if (from_it != graph.node_index.end() && to_it != graph.node_index.end())
        {
            graph.adjacency[from_it->second].push_back({to_it->second, weight});
        }
        // If nodes don't exist, the validator will catch this
    }

    // Algorithms
    if (!payload.contains("algorithms") || !payload["algorithms"].is_array())
    {
        result.ok = false;
        result.error_code = "ERROR_INVALID_REQUEST";
        result.error_message = "Missing or invalid 'payload.algorithms' field.";
        result.error_field = "payload.algorithms";
        return result;
    }

    for (const auto& algo : payload["algorithms"])
    {
        if (!algo.is_string())
        {
            result.ok = false;
            result.error_code = "ERROR_INVALID_REQUEST";
            result.error_message = "Algorithm name must be a string.";
            result.error_field = "payload.algorithms";
            return result;
        }
        result.request.algorithms.push_back(algo.get<std::string>());
    }

    result.ok = true;
    return result;
}

} // namespace hpc

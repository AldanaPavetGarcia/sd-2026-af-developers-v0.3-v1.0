/**
 * @file validator.cpp
 * @brief Implementation of strict graph validation before algorithm processing.
 */

#include "hpc/validator.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace hpc
{

ValidationResult validateGraph(const Graph& graph, const std::vector<std::string>& algorithms)
{
    ValidationResult result;

    // Check: nodes list is not empty
    if (graph.num_nodes == 0)
    {
        result.ok = false;
        result.error_code = "ERROR_INVALID_GRAPH";
        result.error_message = "The nodes list is empty.";
        result.error_field = "payload.nodes";
        return result;
    }

    // Check: no duplicate node identifiers
    {
        std::unordered_set<std::string> seen;
        for (int i = 0; i < graph.num_nodes; ++i)
        {
            if (!seen.insert(graph.node_names[i]).second)
            {
                result.ok = false;
                result.error_code = "ERROR_INVALID_GRAPH";
                result.error_message =
                    "Duplicate node identifier: '" + graph.node_names[i] + "'.";
                result.error_field = "payload.nodes[" + std::to_string(i) + "]";
                return result;
            }
        }
    }

    // Check: all edges reference existing nodes and have valid weights
    int edge_index = 0;
    for (int src = 0; src < graph.num_nodes; ++src)
    {
        for (const auto& edge : graph.adjacency[src])
        {
            // Check target node index bounds
            if (edge.to < 0 || edge.to >= graph.num_nodes)
            {
                result.ok = false;
                result.error_code = "ERROR_INVALID_GRAPH";
                result.error_message =
                    "Edge references a non-existent target node.";
                result.error_field =
                    "payload.edges[" + std::to_string(edge_index) + "].to";
                return result;
            }

            // Check weight: must be positive and finite
            if (std::isnan(edge.weight) || std::isinf(edge.weight))
            {
                result.ok = false;
                result.error_code = "ERROR_INVALID_GRAPH";
                result.error_message =
                    "Edge weight is NaN or Infinity.";
                result.error_field =
                    "payload.edges[" + std::to_string(edge_index) + "].weight";
                return result;
            }

            if (edge.weight <= 0.0)
            {
                result.ok = false;
                result.error_code = "ERROR_INVALID_GRAPH";
                result.error_message =
                    "Edge weight must be a positive number. Got: " +
                    std::to_string(edge.weight) + ".";
                result.error_field =
                    "payload.edges[" + std::to_string(edge_index) + "].weight";
                return result;
            }

            ++edge_index;
        }
    }

    // Check: all requested algorithms are supported
    for (size_t i = 0; i < algorithms.size(); ++i)
    {
        bool found = false;
        for (const auto& supported : SUPPORTED_ALGORITHMS)
        {
            if (algorithms[i] == supported)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            result.ok = false;
            result.error_code = "ERROR_UNSUPPORTED_ALGORITHM";
            result.error_message =
                "Unsupported algorithm: '" + algorithms[i] + "'.";
            result.error_field =
                "payload.algorithms[" + std::to_string(i) + "]";
            return result;
        }
    }

    result.ok = true;
    return result;
}

} // namespace hpc

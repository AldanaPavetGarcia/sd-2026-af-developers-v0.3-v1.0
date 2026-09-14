/**
 * @file validator.cpp
 * @brief Implementation of strict graph validation before algorithm processing.
 */

#include "hpc/validator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace hpc
{

namespace
{

/**
 * @brief Reads a positive integer env var, falling back to defaultValue if unset, empty, or
 * not a valid positive integer. Never throws -- an operator's malformed override degrades to
 * the safe default instead of crashing the engine.
 */
int readPositiveIntEnv(const char* name, int defaultValue)
{
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0')
    {
        return defaultValue;
    }

    try
    {
        const long parsed = std::stol(raw);
        if (parsed > 0 && parsed <= std::numeric_limits<int>::max())
        {
            return static_cast<int>(parsed);
        }
    }
    catch (const std::exception&)
    {
        // Malformed value -- fall through to the default below.
    }

    return defaultValue;
}

} // namespace

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

    // Check: graph doesn't exceed the configured general size limits (ERROR_GRAPH_TOO_LARGE,
    // docs/analyze-graph-protocol-v0.2.md's Size Limits section). Checked before any of the
    // more expensive per-edge validation below, so an oversized request fails fast.
    const int maxNodes = readPositiveIntEnv("EOP_MAX_GRAPH_NODES", kDefaultMaxGraphNodes);
    if (graph.num_nodes > maxNodes)
    {
        result.ok = false;
        result.error_code = "ERROR_GRAPH_TOO_LARGE";
        result.error_message = "Graph has " + std::to_string(graph.num_nodes) +
                                " nodes, which exceeds the configured limit of " +
                                std::to_string(maxNodes) + " (EOP_MAX_GRAPH_NODES).";
        result.error_field = "payload.nodes";
        return result;
    }

    const int maxEdges = readPositiveIntEnv("EOP_MAX_GRAPH_EDGES", kDefaultMaxGraphEdges);
    if (graph.num_edges > maxEdges)
    {
        result.ok = false;
        result.error_code = "ERROR_GRAPH_TOO_LARGE";
        result.error_message = "Graph has " + std::to_string(graph.num_edges) +
                                " edges, which exceeds the configured limit of " +
                                std::to_string(maxEdges) + " (EOP_MAX_GRAPH_EDGES).";
        result.error_field = "payload.edges";
        return result;
    }

    // shortest_paths specifically needs a much stricter cap -- see kDefaultMaxShortestPathsNodes
    // for why (O(N^3) full-path output, confirmed to freeze a host at N=1000).
    const bool requestsShortestPaths =
        std::find(algorithms.begin(), algorithms.end(), "shortest_paths") != algorithms.end();
    if (requestsShortestPaths)
    {
        const int maxShortestPathsNodes =
            readPositiveIntEnv("EOP_MAX_SHORTEST_PATHS_NODES", kDefaultMaxShortestPathsNodes);
        if (graph.num_nodes > maxShortestPathsNodes)
        {
            result.ok = false;
            result.error_code = "ERROR_GRAPH_TOO_LARGE";
            result.error_message =
                "Graph has " + std::to_string(graph.num_nodes) +
                " nodes, which exceeds the configured limit of " +
                std::to_string(maxShortestPathsNodes) +
                " (EOP_MAX_SHORTEST_PATHS_NODES) for the shortest_paths algorithm -- "
                "its all-pairs full-path output grows as O(N^3) and can exhaust memory well "
                "before EOP_MAX_GRAPH_NODES is reached. Request connected_components or "
                "centrality instead for graphs this size, or omit shortest_paths.";
            result.error_field = "payload.algorithms";
            return result;
        }
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

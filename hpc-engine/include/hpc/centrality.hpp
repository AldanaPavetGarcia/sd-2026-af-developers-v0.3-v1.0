#ifndef HPC_CENTRALITY_HPP
#define HPC_CENTRALITY_HPP

/**
 * @file centrality.hpp
 * @brief Betweenness Centrality using Brandes' algorithm with OpenMP.
 */

#include <nlohmann/json.hpp>

#include "graph.hpp"

namespace hpc
{

/**
 * @brief Compute betweenness centrality for all nodes using Brandes' algorithm.
 *
 * Each source node runs an independent BFS + back-propagation.
 * OpenMP parallelizes across source nodes; centrality scores are
 * accumulated using thread-local storage to avoid race conditions.
 *
 * Scores are normalized by (V-1)(V-2) for directed graphs.
 *
 * @param graph The validated graph.
 * @return JSON object with centrality scores: { "node_name": score, ... }
 */
nlohmann::json computeCentrality(const Graph& graph);

} // namespace hpc

#endif // HPC_CENTRALITY_HPP

#ifndef HPC_DIJKSTRA_HPP
#define HPC_DIJKSTRA_HPP

/**
 * @file dijkstra.hpp
 * @brief Parallel all-pairs shortest paths using Dijkstra's algorithm with OpenMP.
 */

#include <nlohmann/json.hpp>

#include "graph.hpp"

namespace hpc
{

/**
 * @brief Compute all-pairs shortest paths using parallel Dijkstra.
 *
 * Each source node runs an independent Dijkstra using a min-heap.
 * OpenMP parallelizes across source nodes with dynamic scheduling.
 *
 * @param graph The validated graph.
 * @return JSON object with shortest path results:
 *         { "src-dst": { "path": [...], "cost": N }, ... }
 */
nlohmann::json computeShortestPaths(const Graph& graph);

} // namespace hpc

#endif // HPC_DIJKSTRA_HPP

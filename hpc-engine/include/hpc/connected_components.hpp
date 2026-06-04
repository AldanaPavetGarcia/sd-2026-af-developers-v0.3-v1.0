#ifndef HPC_CONNECTED_COMPONENTS_HPP
#define HPC_CONNECTED_COMPONENTS_HPP

/**
 * @file connected_components.hpp
 * @brief Connected components detection treating edges as undirected.
 */

#include <nlohmann/json.hpp>

#include "graph.hpp"

namespace hpc
{

/**
 * @brief Find all connected components in the graph.
 *
 * Treats the directed graph as undirected for component detection.
 * Uses BFS with OpenMP parallelization for component labeling.
 *
 * @param graph The validated graph.
 * @return JSON array of arrays, each containing node names in a component.
 */
nlohmann::json computeConnectedComponents(const Graph& graph);

} // namespace hpc

#endif // HPC_CONNECTED_COMPONENTS_HPP

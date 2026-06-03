#ifndef HPC_GRAPH_HPP
#define HPC_GRAPH_HPP

/**
 * @file graph.hpp
 * @brief Internal graph representation using an optimized Adjacency List.
 *
 * Nodes are mapped from string identifiers to integer indices for O(1) access.
 * The adjacency list uses vector<vector<Edge>> for cache-friendly parallel iteration.
 */

#include <string>
#include <unordered_map>
#include <vector>

namespace hpc
{

/**
 * @brief Represents a directed edge in the graph with a weight.
 */
struct Edge
{
    int to;        ///< Target node index in the adjacency list.
    double weight; ///< Positive finite edge weight.
};

/**
 * @brief Internal graph representation optimized for HPC processing.
 *
 * String node identifiers are mapped to contiguous integer indices
 * to enable direct array indexing in algorithm implementations.
 */
struct Graph
{
    int num_nodes = 0; ///< Total number of nodes.
    int num_edges = 0; ///< Total number of edges.

    std::vector<std::string> node_names;             ///< Index → node name mapping.
    std::unordered_map<std::string, int> node_index;  ///< Node name → index mapping.
    std::vector<std::vector<Edge>> adjacency;         ///< Adjacency list (directed).
};

} // namespace hpc

#endif // HPC_GRAPH_HPP

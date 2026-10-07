#ifndef HPC_VALIDATOR_HPP
#define HPC_VALIDATOR_HPP

/**
 * @file validator.hpp
 * @brief Graph validation layer — verifies topology integrity before processing.
 *
 * Implements strict validation per US-202 acceptance criteria:
 * - Non-empty nodes list
 * - Unique node identifiers
 * - Edge references point to existing nodes
 * - Edge weights are positive finite numbers
 * - Requested algorithms are supported
 */

#include <string>

#include "graph.hpp"

namespace hpc
{

/**
 * @brief Result of a graph validation check.
 */
struct ValidationResult
{
    bool ok = true;
    std::string error_code;
    std::string error_message;
    std::string error_field;
};

/**
 * @brief Supported algorithm names.
 */
inline const std::vector<std::string> SUPPORTED_ALGORITHMS = {
    "shortest_paths",
    "connected_components",
    "centrality"};

/**
 * @brief Default maximum node count (ERROR_GRAPH_TOO_LARGE), overridable via
 * EOP_MAX_GRAPH_NODES. Matches the PRD's stated scope of graphs up to 100K nodes
 * (ADR-007 -- connected_components/centrality are both proven to handle this scale).
 */
constexpr int kDefaultMaxGraphNodes = 100000;

/**
 * @brief Default maximum edge count (ERROR_GRAPH_TOO_LARGE), overridable via
 * EOP_MAX_GRAPH_EDGES.
 */
constexpr int kDefaultMaxGraphEdges = 500000;

/**
 * @brief Default maximum node count specifically for `shortest_paths`, overridable via
 * EOP_MAX_SHORTEST_PATHS_NODES (not one of the two keys the protocol doc names -- added
 * because of a real, confirmed incident, see below).
 *
 * `computeShortestPaths()` is an all-pairs algorithm that returns the FULL PATH (not just the
 * cost) for every reachable node pair -- summed across all pairs, the total number of
 * (pair, path-node) entries is O(N^3). At N=1000 this reached 6GB resident memory and climbing
 * before it had to be killed, freezing the host it ran on (see hpc-engine/tests/test_medium_graph.cpp
 * and its regression test, capped to 50 nodes for exactly this reason). kDefaultMaxGraphNodes
 * (100K) is nowhere near safe for this specific algorithm -- it's sized for
 * connected_components/centrality, which don't carry this risk.
 *
 * 300 nodes keeps the worst-case total path-length sum (N^3/6 for a fully-connected graph)
 * under ~4.5 million entries -- comfortably fast and low-memory. This is deliberately much
 * smaller than the general graph size limit; that asymmetry is intentional, not an oversight.
 */
constexpr int kDefaultMaxShortestPathsNodes = 300;

/**
 * @brief Validate a parsed graph and algorithm list.
 *
 * Must be called after parseRequest() and before running any algorithm.
 * On failure, returns a ValidationResult with structured error details
 * suitable for building an ERROR response.
 *
 * @param graph The graph to validate.
 * @param algorithms The list of requested algorithm names.
 * @return ValidationResult with ok=true if valid, or error details.
 */
ValidationResult validateGraph(const Graph& graph, const std::vector<std::string>& algorithms);

} // namespace hpc

#endif // HPC_VALIDATOR_HPP

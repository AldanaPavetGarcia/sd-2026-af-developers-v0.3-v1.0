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

    /// Default node count above which centrality switches to pivot sampling (see ADR-019, option
    /// D): US-201 AC3 names 10,000 nodes and is met exactly at that size, so the threshold sits
    /// there rather than below it -- sampling is for graphs larger than any AC's latency figure.
    constexpr int kDefaultCentralityExactMaxNodes = 10000;

    /// Default number of sampled source nodes used once the exact threshold is exceeded.
    constexpr int kDefaultCentralitySampleCount = 1024;

    /**
     * @brief How a centrality result was produced.
     *
     * @details Callers need this to avoid presenting an estimate as an exact figure: above a
     * configured graph size the scores come from a sampled subset of source nodes, not from all of
     * them (ADR-019).
     */
    struct CentralityStats
    {
        bool approximate = false; ///< True when pivot sampling was used instead of every source.
        int sampled_sources = 0;  ///< Source nodes actually traversed (equals node count if exact).
        int total_sources = 0;    ///< Source nodes an exact run would have traversed.
    };

    /**
     * @brief Compute betweenness centrality for all nodes using Brandes' algorithm.
     *
     * Each source node runs an independent BFS + back-propagation.
     * OpenMP parallelizes across source nodes; centrality scores are
     * accumulated using thread-local storage to avoid race conditions.
     *
     * Scores are normalized by (V-1)(V-2) for directed graphs.
     *
     * @details Exact for graphs up to `EOP_HPC_CENTRALITY_EXACT_MAX_NODES` nodes (default
     * kDefaultCentralityExactMaxNodes, which covers US-201 AC3's 10,000-node case exactly).
     * Above that, betweenness is estimated from `EOP_HPC_CENTRALITY_SAMPLES` sampled source nodes
     * and rescaled, because exact Brandes is O(V*E) and grows to tens of minutes per request at
     * the 100K-node graphs US-202 benchmarks. Pivots are chosen by a partial Fisher-Yates shuffle
     * driven by a fixed seed, so repeated runs on the same graph return identical scores. See
     * ADR-019 for the trade-off and the accuracy this implies.
     *
     * @param graph The validated graph.
     * @return JSON object with centrality scores: { "node_name": score, ... }
     */
    nlohmann::json computeCentrality(const Graph& graph);

    /**
     * @brief Same as computeCentrality(const Graph&), additionally reporting how it was computed.
     *
     * @param graph The validated graph.
     * @param[out] stats Filled with whether sampling was used and how many sources were traversed.
     * @return JSON object with centrality scores: { "node_name": score, ... }
     */
    nlohmann::json computeCentrality(const Graph& graph, CentralityStats& stats);

} // namespace hpc

#endif // HPC_CENTRALITY_HPP

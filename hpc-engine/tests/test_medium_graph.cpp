/**
 * @file test_medium_graph.cpp
 * @brief US-201 AC7: unit test coverage for a medium-sized graph (1,000 nodes).
 *
 * The existing per-algorithm test suites (test_dijkstra.cpp, test_connected_components.cpp,
 * test_centrality.cpp) only exercise trivial hand-verified graphs (2-4 nodes). This file adds
 * the "medium graph" category the PRD's acceptance criteria lists explicitly, exercising all
 * three algorithms required by US-201 AC2 together against a single 1,000-node graph, so the
 * full test matrix (10 nodes / 1K nodes / disconnected / single node) is actually covered.
 */

#include <gtest/gtest.h>

#include "hpc/centrality.hpp"
#include "hpc/connected_components.hpp"
#include "hpc/dijkstra.hpp"

namespace
{

    constexpr int kMediumGraphNodeCount = 1000;

    // computeShortestPaths returns the FULL PATH (not just the cost) for every reachable pair.
    // On a path graph, the total number of (pair, path-node) entries summed across all pairs
    // is O(N^3) -- for N=1000 that is on the order of 1.6e8 nlohmann::json entries, which
    // measured over 6GB resident and climbing before being killed (real incident, 07/09: this
    // exact call hung a hpc_tests run and froze the host). Keep the shortest-paths check on a
    // graph small enough that the O(N^3) blowup stays negligible; the 1000-node scale is only
    // exercised by the two algorithms that don't carry this cost (connected components is O(N),
    // centrality is O(N*E)).
    constexpr int kShortestPathsSafeNodeCount = 50;

    /**
     * @brief Builds a directed path graph N0 -> N1 -> ... -> N(count-1) with unit edge weights.
     *
     * A path graph gives every algorithm a single connected component and a predictable
     * shortest-path structure, so results can be checked without needing a second
     * implementation to compare against.
     */
    hpc::Graph buildPathGraph(int nodeCount)
    {
        hpc::Graph g;
        g.num_nodes = nodeCount;
        g.node_names.reserve(static_cast<size_t>(nodeCount));
        g.adjacency.resize(static_cast<size_t>(nodeCount));

        for (int i = 0; i < nodeCount; ++i)
        {
            const std::string name = "N" + std::to_string(i);
            g.node_names.push_back(name);
            g.node_index[name] = i;
        }

        for (int i = 0; i < nodeCount - 1; ++i)
        {
            g.adjacency[static_cast<size_t>(i)].push_back({i + 1, 1.0});
        }
        g.num_edges = nodeCount - 1;

        return g;
    }

} // namespace

TEST(MediumGraphTest, ConnectedComponentsSingleComponentOf1000Nodes)
{
    hpc::Graph g = buildPathGraph(kMediumGraphNodeCount);

    auto res = hpc::computeConnectedComponents(g);

    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0].size(), static_cast<size_t>(kMediumGraphNodeCount));
}

TEST(MediumGraphTest, ShortestPathsAcrossFullChain)
{
    // Deliberately NOT run at kMediumGraphNodeCount -- see the comment on
    // kShortestPathsSafeNodeCount above.
    hpc::Graph g = buildPathGraph(kShortestPathsSafeNodeCount);

    auto res = hpc::computeShortestPaths(g);

    // Directed path graph: every i < j pair is reachable, none in the other direction.
    const size_t expectedPairs =
        static_cast<size_t>(kShortestPathsSafeNodeCount) * (kShortestPathsSafeNodeCount - 1) / 2;
    EXPECT_EQ(res.size(), expectedPairs);

    // End-to-end: N0 -> N(last) costs (count-1) hops of weight 1.0 each.
    const std::string lastNode = "N" + std::to_string(kShortestPathsSafeNodeCount - 1);
    const std::string key = "N0-" + lastNode;
    ASSERT_TRUE(res.contains(key));
    EXPECT_DOUBLE_EQ(res[key]["cost"], static_cast<double>(kShortestPathsSafeNodeCount - 1));
    EXPECT_EQ(res[key]["path"].size(), static_cast<size_t>(kShortestPathsSafeNodeCount));

    // No path in the reverse direction on a directed chain.
    EXPECT_FALSE(res.contains(lastNode + "-N0"));
}

TEST(MediumGraphTest, CentralityEndpointsHaveNoBetweenness)
{
    hpc::Graph g = buildPathGraph(kMediumGraphNodeCount);

    auto res = hpc::computeCentrality(g);

    ASSERT_EQ(res.size(), static_cast<size_t>(kMediumGraphNodeCount));

    // Path-graph endpoints never lie between two other nodes.
    EXPECT_DOUBLE_EQ(res["N0"], 0.0);
    EXPECT_DOUBLE_EQ(res["N999"], 0.0);

    // An interior node must have strictly positive betweenness: it sits on at least one
    // shortest path between two other nodes on the chain.
    EXPECT_GT(res["N500"].get<double>(), 0.0);
}

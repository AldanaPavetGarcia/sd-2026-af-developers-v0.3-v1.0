/**
 * @file test_centrality.cpp
 * @brief Unit tests for betweenness centrality algorithm.
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "hpc/centrality.hpp"

TEST(CentralityTest, LineGraph)
{
    // A - B - C
    hpc::Graph g;
    g.num_nodes = 3;
    g.node_names = {"A", "B", "C"};
    g.node_index = {{"A", 0}, {"B", 1}, {"C", 2}};
    g.adjacency.resize(3);
    g.adjacency[0].push_back({1, 1.0}); // A -> B
    g.adjacency[1].push_back({0, 1.0}); // B -> A
    g.adjacency[1].push_back({2, 1.0}); // B -> C
    g.adjacency[2].push_back({1, 1.0}); // C -> B

    auto res = hpc::computeCentrality(g);

    // Normalization factor: (3-1)*(3-2) = 2
    // B lies on 2 shortest paths (A->C, C->A), so raw score is 2.
    // Normalized score is 2 / 2 = 1.0
    EXPECT_DOUBLE_EQ(res["B"], 1.0);

    // A and C lie on no shortest paths between other nodes
    EXPECT_DOUBLE_EQ(res["A"], 0.0);
    EXPECT_DOUBLE_EQ(res["C"], 0.0);
}

TEST(CentralityTest, StarGraph)
{
    // A -> B, C -> B, D -> B
    // Central node is B
    hpc::Graph g;
    g.num_nodes = 4;
    g.node_names = {"A", "B", "C", "D"};
    g.node_index = {{"A", 0}, {"B", 1}, {"C", 2}, {"D", 3}};
    g.adjacency.resize(4);

    // Add edges to make it an undirected star
    g.adjacency[0].push_back({1, 1.0});
    g.adjacency[1].push_back({0, 1.0});
    g.adjacency[2].push_back({1, 1.0});
    g.adjacency[1].push_back({2, 1.0});
    g.adjacency[3].push_back({1, 1.0});
    g.adjacency[1].push_back({3, 1.0});

    auto res = hpc::computeCentrality(g);

    // Normalization factor: (4-1)*(4-2) = 6
    // B lies on paths: A-C, C-A, A-D, D-A, C-D, D-C (6 paths)
    // Normalized score: 6 / 6 = 1.0
    EXPECT_DOUBLE_EQ(res["B"], 1.0);
    EXPECT_DOUBLE_EQ(res["A"], 0.0);
    EXPECT_DOUBLE_EQ(res["C"], 0.0);
    EXPECT_DOUBLE_EQ(res["D"], 0.0);
}

TEST(CentralityTest, SingleNode)
{
    hpc::Graph g;
    g.num_nodes = 1;
    g.node_names = {"A"};
    g.node_index = {{"A", 0}};
    g.adjacency.resize(1);

    auto res = hpc::computeCentrality(g);

    // Normalization for n=1 is 1.0. Score is 0.0.
    EXPECT_DOUBLE_EQ(res["A"], 0.0);
}

// ---------------------------------------------------------------------------
// Pivot sampling for large graphs (ADR-019 / US-201 AC3)
// ---------------------------------------------------------------------------

namespace
{
    /// Builds a ring of `n` nodes with bidirectional edges, so every node is reachable and no
    /// node is structurally special — sampling behaviour is then the only thing under test.
    hpc::Graph makeRing(int n)
    {
        hpc::Graph g;
        g.num_nodes = n;
        g.adjacency.resize(static_cast<std::size_t>(n));

        for (int i = 0; i < n; ++i)
        {
            const std::string name = "N" + std::to_string(i);
            g.node_names.push_back(name);
            g.node_index[name] = i;
        }

        for (int i = 0; i < n; ++i)
        {
            const int next = (i + 1) % n;
            g.adjacency[static_cast<std::size_t>(i)].push_back({next, 1.0});
            g.adjacency[static_cast<std::size_t>(next)].push_back({i, 1.0});
            g.num_edges += 2;
        }

        return g;
    }
} // namespace

TEST(CentralitySamplingTest, SmallGraphStaysExact)
{
    unsetenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES");
    unsetenv("EOP_HPC_CENTRALITY_SAMPLES");

    const hpc::Graph g = makeRing(50);

    hpc::CentralityStats stats;
    auto res = hpc::computeCentrality(g, stats);

    EXPECT_FALSE(stats.approximate);
    EXPECT_EQ(stats.sampled_sources, 50);
    EXPECT_EQ(stats.total_sources, 50);
    EXPECT_EQ(res.size(), 50u);
}

TEST(CentralitySamplingTest, AboveThresholdSamplesSources)
{
    setenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES", "20", 1);
    setenv("EOP_HPC_CENTRALITY_SAMPLES", "8", 1);

    const hpc::Graph g = makeRing(60);

    hpc::CentralityStats stats;
    auto res = hpc::computeCentrality(g, stats);

    EXPECT_TRUE(stats.approximate);
    EXPECT_EQ(stats.sampled_sources, 8);
    EXPECT_EQ(stats.total_sources, 60);
    // Every node still gets a score; only how it was estimated changed.
    EXPECT_EQ(res.size(), 60u);

    unsetenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES");
    unsetenv("EOP_HPC_CENTRALITY_SAMPLES");
}

TEST(CentralitySamplingTest, SamplingIsDeterministic)
{
    setenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES", "20", 1);
    setenv("EOP_HPC_CENTRALITY_SAMPLES", "8", 1);

    const hpc::Graph g = makeRing(60);

    // Same graph, same configuration: identical scores, or a demo/test could not rely on them.
    auto first = hpc::computeCentrality(g);
    auto second = hpc::computeCentrality(g);
    EXPECT_EQ(first, second);

    unsetenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES");
    unsetenv("EOP_HPC_CENTRALITY_SAMPLES");
}

TEST(CentralitySamplingTest, SamplingEveryNodeIsReportedAsExact)
{
    // Asking for at least as many samples as there are nodes is the exact algorithm, and must
    // not be labelled an estimate.
    setenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES", "10", 1);
    setenv("EOP_HPC_CENTRALITY_SAMPLES", "500", 1);

    const hpc::Graph g = makeRing(40);

    hpc::CentralityStats stats;
    hpc::computeCentrality(g, stats);

    EXPECT_FALSE(stats.approximate);
    EXPECT_EQ(stats.sampled_sources, 40);

    unsetenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES");
    unsetenv("EOP_HPC_CENTRALITY_SAMPLES");
}

TEST(CentralitySamplingTest, MalformedConfigFallsBackToDefaults)
{
    setenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES", "not-a-number", 1);
    setenv("EOP_HPC_CENTRALITY_SAMPLES", "-5", 1);

    // 30 nodes is below the default exact threshold, so a correct fallback yields an exact run.
    const hpc::Graph g = makeRing(30);

    hpc::CentralityStats stats;
    hpc::computeCentrality(g, stats);

    EXPECT_FALSE(stats.approximate);
    EXPECT_EQ(stats.sampled_sources, 30);

    unsetenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES");
    unsetenv("EOP_HPC_CENTRALITY_SAMPLES");
}

TEST(CentralitySamplingTest, EstimateIsScaledToExactMagnitude)
{
    // On a ring every node has identical betweenness, so an unbiased estimator must land close
    // to the exact value -- this catches a missing or wrong V/k rescaling, which would otherwise
    // shrink every score by the sampling fraction.
    unsetenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES");
    unsetenv("EOP_HPC_CENTRALITY_SAMPLES");
    const hpc::Graph g = makeRing(120);
    auto exact = hpc::computeCentrality(g);

    setenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES", "20", 1);
    setenv("EOP_HPC_CENTRALITY_SAMPLES", "40", 1);
    auto estimate = hpc::computeCentrality(g);

    const double exact_value = exact["N0"].get<double>();
    const double estimated_value = estimate["N0"].get<double>();

    ASSERT_GT(exact_value, 0.0);
    EXPECT_NEAR(estimated_value, exact_value, exact_value * 0.25);

    unsetenv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES");
    unsetenv("EOP_HPC_CENTRALITY_SAMPLES");
}

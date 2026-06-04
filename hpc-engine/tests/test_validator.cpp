/**
 * @file test_validator.cpp
 * @brief Unit tests for the graph validation layer.
 *
 * Covers: empty nodes, duplicate nodes, invalid edge refs, negative/NaN/Inf weights,
 * unsupported algorithms.
 */

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "hpc/validator.hpp"

// Helper to build a simple valid graph
static hpc::Graph makeValidGraph()
{
    hpc::Graph g;
    g.num_nodes = 3;
    g.node_names = {"A", "B", "C"};
    g.node_index = {{"A", 0}, {"B", 1}, {"C", 2}};
    g.adjacency.resize(3);
    g.adjacency[0].push_back({1, 10.0});
    g.adjacency[1].push_back({2, 5.0});
    g.num_edges = 2;
    return g;
}

TEST(ValidatorTest, ValidGraphPasses)
{
    auto g = makeValidGraph();
    auto r = hpc::validateGraph(g, {"shortest_paths", "connected_components", "centrality"});
    EXPECT_TRUE(r.ok);
}

TEST(ValidatorTest, EmptyNodesListFails)
{
    hpc::Graph g;
    g.num_nodes = 0;
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, DuplicateNodesFails)
{
    hpc::Graph g;
    g.num_nodes = 2;
    g.node_names = {"A", "A"};
    g.node_index = {{"A", 0}};
    g.adjacency.resize(2);
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, NegativeWeightFails)
{
    auto g = makeValidGraph();
    g.adjacency[0][0].weight = -1.0;
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, ZeroWeightFails)
{
    auto g = makeValidGraph();
    g.adjacency[0][0].weight = 0.0;
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, NaNWeightFails)
{
    auto g = makeValidGraph();
    g.adjacency[0][0].weight = std::numeric_limits<double>::quiet_NaN();
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, InfinityWeightFails)
{
    auto g = makeValidGraph();
    g.adjacency[0][0].weight = std::numeric_limits<double>::infinity();
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, UnsupportedAlgorithmFails)
{
    auto g = makeValidGraph();
    auto r = hpc::validateGraph(g, {"shortest_paths", "unknown_algo"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_UNSUPPORTED_ALGORITHM");
}

TEST(ValidatorTest, InvalidEdgeTargetFails)
{
    hpc::Graph g;
    g.num_nodes = 2;
    g.node_names = {"A", "B"};
    g.node_index = {{"A", 0}, {"B", 1}};
    g.adjacency.resize(2);
    g.adjacency[0].push_back({99, 1.0}); // target index out of bounds
    g.num_edges = 1;
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, SingleNodeGraphIsValid)
{
    hpc::Graph g;
    g.num_nodes = 1;
    g.node_names = {"solo"};
    g.node_index = {{"solo", 0}};
    g.adjacency.resize(1);
    g.num_edges = 0;
    auto r = hpc::validateGraph(g, {"connected_components"});
    EXPECT_TRUE(r.ok);
}

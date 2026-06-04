/**
 * @file test_graph.cpp
 * @brief Unit tests for Graph data structure construction and node index mapping.
 */

#include <gtest/gtest.h>

#include "hpc/graph.hpp"

TEST(GraphTest, EmptyGraphHasZeroCounts)
{
    hpc::Graph g;
    EXPECT_EQ(g.num_nodes, 0);
    EXPECT_EQ(g.num_edges, 0);
    EXPECT_TRUE(g.node_names.empty());
    EXPECT_TRUE(g.adjacency.empty());
}

TEST(GraphTest, NodeIndexMappingIsCorrect)
{
    hpc::Graph g;
    g.num_nodes = 3;
    g.node_names = {"A", "B", "C"};
    g.node_index = {{"A", 0}, {"B", 1}, {"C", 2}};
    g.adjacency.resize(3);

    EXPECT_EQ(g.node_index.at("A"), 0);
    EXPECT_EQ(g.node_index.at("B"), 1);
    EXPECT_EQ(g.node_index.at("C"), 2);
    EXPECT_EQ(g.node_names[0], "A");
}

TEST(GraphTest, AdjacencyListStoresEdges)
{
    hpc::Graph g;
    g.num_nodes = 2;
    g.node_names = {"X", "Y"};
    g.node_index = {{"X", 0}, {"Y", 1}};
    g.adjacency.resize(2);
    g.adjacency[0].push_back({1, 5.0});
    g.num_edges = 1;

    EXPECT_EQ(g.adjacency[0].size(), 1u);
    EXPECT_EQ(g.adjacency[0][0].to, 1);
    EXPECT_DOUBLE_EQ(g.adjacency[0][0].weight, 5.0);
    EXPECT_TRUE(g.adjacency[1].empty());
}

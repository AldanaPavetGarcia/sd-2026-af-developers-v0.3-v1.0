/**
 * @file test_centrality.cpp
 * @brief Unit tests for betweenness centrality algorithm.
 */

#include <gtest/gtest.h>

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

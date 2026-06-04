/**
 * @file test_dijkstra.cpp
 * @brief Unit tests for parallel Dijkstra shortest paths algorithm.
 */

#include <gtest/gtest.h>

#include "hpc/dijkstra.hpp"

TEST(DijkstraTest, SimplePath)
{
    hpc::Graph g;
    g.num_nodes = 3;
    g.node_names = {"A", "B", "C"};
    g.node_index = {{"A", 0}, {"B", 1}, {"C", 2}};
    g.adjacency.resize(3);
    g.adjacency[0].push_back({1, 10.0});
    g.adjacency[1].push_back({2, 5.0});

    auto res = hpc::computeShortestPaths(g);

    // A -> B
    EXPECT_TRUE(res.contains("A-B"));
    EXPECT_EQ(res["A-B"]["cost"], 10.0);
    EXPECT_EQ(res["A-B"]["path"], nlohmann::json::array({"A", "B"}));

    // B -> C
    EXPECT_TRUE(res.contains("B-C"));
    EXPECT_EQ(res["B-C"]["cost"], 5.0);
    EXPECT_EQ(res["B-C"]["path"], nlohmann::json::array({"B", "C"}));

    // A -> C
    EXPECT_TRUE(res.contains("A-C"));
    EXPECT_EQ(res["A-C"]["cost"], 15.0);
    EXPECT_EQ(res["A-C"]["path"], nlohmann::json::array({"A", "B", "C"}));

    // No path C -> A
    EXPECT_FALSE(res.contains("C-A"));
}

TEST(DijkstraTest, DisconnectedGraph)
{
    hpc::Graph g;
    g.num_nodes = 2;
    g.node_names = {"A", "B"};
    g.node_index = {{"A", 0}, {"B", 1}};
    g.adjacency.resize(2);

    auto res = hpc::computeShortestPaths(g);

    EXPECT_TRUE(res.empty());
}

TEST(DijkstraTest, SelfLoopIgnored)
{
    hpc::Graph g;
    g.num_nodes = 1;
    g.node_names = {"A"};
    g.node_index = {{"A", 0}};
    g.adjacency.resize(1);
    g.adjacency[0].push_back({0, 1.0});

    auto res = hpc::computeShortestPaths(g);

    // Source == Dest is not reported
    EXPECT_TRUE(res.empty());
}

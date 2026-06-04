/**
 * @file test_connected_components.cpp
 * @brief Unit tests for connected components detection.
 */

#include <gtest/gtest.h>

#include "hpc/connected_components.hpp"

TEST(ConnectedComponentsTest, SingleComponent)
{
    hpc::Graph g;
    g.num_nodes = 3;
    g.node_names = {"A", "B", "C"};
    g.node_index = {{"A", 0}, {"B", 1}, {"C", 2}};
    g.adjacency.resize(3);
    g.adjacency[0].push_back({1, 1.0});
    g.adjacency[1].push_back({2, 1.0});

    auto res = hpc::computeConnectedComponents(g);

    EXPECT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0], nlohmann::json::array({"A", "B", "C"}));
}

TEST(ConnectedComponentsTest, MultipleComponents)
{
    hpc::Graph g;
    g.num_nodes = 4;
    g.node_names = {"A", "B", "C", "D"};
    g.node_index = {{"A", 0}, {"B", 1}, {"C", 2}, {"D", 3}};
    g.adjacency.resize(4);
    g.adjacency[0].push_back({1, 1.0}); // A -> B
    g.adjacency[2].push_back({3, 1.0}); // C -> D

    auto res = hpc::computeConnectedComponents(g);

    EXPECT_EQ(res.size(), 2u);
    // The order of components in the outer array is not guaranteed,
    // so we just check that both expected components are present.
    bool foundAB = false;
    bool foundCD = false;
    for (const auto& comp : res)
    {
        if (comp == nlohmann::json::array({"A", "B"})) foundAB = true;
        if (comp == nlohmann::json::array({"C", "D"})) foundCD = true;
    }
    EXPECT_TRUE(foundAB);
    EXPECT_TRUE(foundCD);
}

TEST(ConnectedComponentsTest, DisconnectedNodes)
{
    hpc::Graph g;
    g.num_nodes = 3;
    g.node_names = {"X", "Y", "Z"};
    g.node_index = {{"X", 0}, {"Y", 1}, {"Z", 2}};
    g.adjacency.resize(3);

    auto res = hpc::computeConnectedComponents(g);

    EXPECT_EQ(res.size(), 3u);
    bool foundX = false, foundY = false, foundZ = false;
    for (const auto& comp : res)
    {
        if (comp == nlohmann::json::array({"X"})) foundX = true;
        if (comp == nlohmann::json::array({"Y"})) foundY = true;
        if (comp == nlohmann::json::array({"Z"})) foundZ = true;
    }
    EXPECT_TRUE(foundX && foundY && foundZ);
}

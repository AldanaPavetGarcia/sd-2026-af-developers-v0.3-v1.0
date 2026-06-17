/**
 * @file test_concurrent_algorithms.cpp
 * @brief US202.AC7 — Concurrency stress test for HPC algorithms under ThreadSanitizer.
 *
 * Launches 10 threads, each executing 100 cycles of:
 *   connect (build graph) → send (invoke algorithm) → disconnect (discard result)
 *
 * All three parallelized algorithms are exercised:
 *   - computeShortestPaths  (Dijkstra, omp parallel for)
 *   - computeCentrality     (Brandes, omp parallel + omp for)
 *   - computeConnectedComponents (omp parallel for)
 *
 * ThreadSanitizer must report zero data races for this test to pass.
 */

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "hpc/centrality.hpp"
#include "hpc/connected_components.hpp"
#include "hpc/dijkstra.hpp"
#include "hpc/graph.hpp"

namespace
{

/// Builds a small but non-trivial directed graph for use in all algorithm calls.
hpc::Graph makeTestGraph()
{
    hpc::Graph g;
    g.num_nodes = 5;
    g.node_names = {"N0", "N1", "N2", "N3", "N4"};
    g.node_index = {{"N0", 0}, {"N1", 1}, {"N2", 2}, {"N3", 3}, {"N4", 4}};
    g.adjacency.resize(5);

    // A small connected DAG-like structure
    g.adjacency[0].push_back({1, 1.0});
    g.adjacency[0].push_back({2, 4.0});
    g.adjacency[1].push_back({2, 2.0});
    g.adjacency[1].push_back({3, 5.0});
    g.adjacency[2].push_back({3, 1.0});
    g.adjacency[2].push_back({4, 3.0});
    g.adjacency[3].push_back({4, 2.0});

    return g;
}

constexpr int kThreads = 10;
constexpr int kCyclesPerThread = 100;

} // namespace

// ---------------------------------------------------------------------------
// US202.AC7 — Dijkstra concurrency
// ---------------------------------------------------------------------------
TEST(ConcurrentAlgorithmsTest, DijkstraConcurrent10Threads100Cycles)
{
    std::atomic<int> failures{0};

    auto worker = [&]()
    {
        for (int cycle = 0; cycle < kCyclesPerThread; ++cycle)
        {
            hpc::Graph g = makeTestGraph();
            auto result = hpc::computeShortestPaths(g);

            // Basic sanity: N0→N1 must always exist with cost 1.0
            if (!result.contains("N0-N1") || result["N0-N1"]["cost"] != 1.0)
            {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back(worker);
    }
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(failures.load(), 0) << "Dijkstra produced incorrect results under concurrent load";
}

// ---------------------------------------------------------------------------
// US202.AC7 — Betweenness Centrality concurrency
// ---------------------------------------------------------------------------
TEST(ConcurrentAlgorithmsTest, CentralityConcurrent10Threads100Cycles)
{
    std::atomic<int> failures{0};

    auto worker = [&]()
    {
        for (int cycle = 0; cycle < kCyclesPerThread; ++cycle)
        {
            hpc::Graph g = makeTestGraph();
            auto result = hpc::computeCentrality(g);

            // Result must contain an entry for every node
            if (static_cast<int>(result.size()) != g.num_nodes)
            {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back(worker);
    }
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(failures.load(), 0) << "Centrality produced incorrect results under concurrent load";
}

// ---------------------------------------------------------------------------
// US202.AC7 — Connected Components concurrency
// ---------------------------------------------------------------------------
TEST(ConcurrentAlgorithmsTest, ConnectedComponentsConcurrent10Threads100Cycles)
{
    std::atomic<int> failures{0};

    auto worker = [&]()
    {
        for (int cycle = 0; cycle < kCyclesPerThread; ++cycle)
        {
            hpc::Graph g = makeTestGraph();
            auto result = hpc::computeConnectedComponents(g);

            // The test graph is fully connected (undirected) — expect 1 component
            if (result.size() != 1)
            {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back(worker);
    }
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(failures.load(), 0) << "ConnectedComponents produced incorrect results under concurrent load";
}

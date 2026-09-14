/**
 * @file centrality.cpp
 * @brief Betweenness Centrality using Brandes' algorithm with OpenMP parallelization.
 *
 * Each source node runs an independent BFS + back-propagation.
 * Thread-local centrality accumulators avoid race conditions.
 * Final scores are normalized by (V-1)(V-2) for directed graphs.
 */

#include "hpc/centrality.hpp"

#include "hpc/metrics.hpp"

#include <queue>
#include <stack>
#include <vector>

#include <omp.h>

namespace hpc
{

nlohmann::json computeCentrality(const Graph& graph)
{
    const int n = graph.num_nodes;

    // Global centrality scores (accumulated from all threads)
    std::vector<double> centrality(n, 0.0);

    // Determine number of threads for thread-local storage
    int max_threads = omp_get_max_threads();
    std::vector<std::vector<double>> thread_centrality(
        max_threads, std::vector<double>(n, 0.0));

#pragma omp parallel
    {
        hpc::ActiveThreadScope activeThreadScope;

        int tid = omp_get_thread_num();

        // Per-thread Brandes data structures (reused across sources)
        std::vector<std::vector<int>> predecessors(n);
        std::vector<double> sigma(n);
        std::vector<double> delta(n);
        std::vector<int> dist(n);
        std::stack<int> stack;
        std::queue<int> bfs_queue;

#pragma omp for schedule(dynamic)
        for (int s = 0; s < n; ++s)
        {
            // Initialize
            for (int i = 0; i < n; ++i)
            {
                predecessors[i].clear();
                sigma[i] = 0.0;
                delta[i] = 0.0;
                dist[i] = -1;
            }

            sigma[s] = 1.0;
            dist[s] = 0;
            bfs_queue.push(s);

            // BFS phase: discover shortest paths
            while (!bfs_queue.empty())
            {
                int v = bfs_queue.front();
                bfs_queue.pop();
                stack.push(v);

                for (const auto& edge : graph.adjacency[v])
                {
                    int w = edge.to;

                    // First visit to w?
                    if (dist[w] < 0)
                    {
                        dist[w] = dist[v] + 1;
                        bfs_queue.push(w);
                    }

                    // Shortest path to w via v?
                    if (dist[w] == dist[v] + 1)
                    {
                        sigma[w] += sigma[v];
                        predecessors[w].push_back(v);
                    }
                }
            }

            // Back-propagation phase: accumulate pair dependencies
            while (!stack.empty())
            {
                int w = stack.top();
                stack.pop();

                for (int v : predecessors[w])
                {
                    delta[v] += (sigma[v] / sigma[w]) * (1.0 + delta[w]);
                }

                if (w != s)
                {
                    thread_centrality[tid][w] += delta[w];
                }
            }
        }
    }

    // Merge thread-local results
    for (int t = 0; t < max_threads; ++t)
    {
        for (int i = 0; i < n; ++i)
        {
            centrality[i] += thread_centrality[t][i];
        }
    }

    // Normalize by (V-1)(V-2) for directed graphs
    double normalization = (n > 2) ? static_cast<double>((n - 1) * (n - 2)) : 1.0;

    nlohmann::json result = nlohmann::json::object();
    for (int i = 0; i < n; ++i)
    {
        result[graph.node_names[i]] = centrality[i] / normalization;
    }

    return result;
}

} // namespace hpc

/**
 * @file dijkstra.cpp
 * @brief Parallel all-pairs shortest paths using Dijkstra's algorithm with OpenMP.
 */

#include "hpc/dijkstra.hpp"

#include "hpc/metrics.hpp"

#include <limits>
#include <queue>
#include <vector>

#include <omp.h>

namespace hpc
{

/**
 * @brief Single-source Dijkstra from a given source node.
 *
 * @param graph   The graph to traverse.
 * @param source  Source node index.
 * @param dist    Output: shortest distances from source to all nodes.
 * @param prev    Output: predecessor indices for path reconstruction.
 */
static void dijkstraSingleSource(
    const Graph& graph,
    int source,
    std::vector<double>& dist,
    std::vector<int>& prev)
{
    const int n = graph.num_nodes;
    dist.assign(n, std::numeric_limits<double>::infinity());
    prev.assign(n, -1);
    dist[source] = 0.0;

    // Min-heap: (distance, node_index)
    using Pair = std::pair<double, int>;
    std::priority_queue<Pair, std::vector<Pair>, std::greater<Pair>> pq;
    pq.push({0.0, source});

    while (!pq.empty())
    {
        auto [d, u] = pq.top();
        pq.pop();

        if (d > dist[u])
        {
            continue;
        }

        for (const auto& edge : graph.adjacency[u])
        {
            double new_dist = dist[u] + edge.weight;
            if (new_dist < dist[edge.to])
            {
                dist[edge.to] = new_dist;
                prev[edge.to] = u;
                pq.push({new_dist, edge.to});
            }
        }
    }
}

/**
 * @brief Reconstruct the path from source to target using predecessor array.
 */
static std::vector<int> reconstructPath(const std::vector<int>& prev, int source, int target)
{
    std::vector<int> path;
    for (int at = target; at != -1; at = prev[at])
    {
        path.push_back(at);
        if (at == source)
        {
            break;
        }
    }
    std::reverse(path.begin(), path.end());

    // Verify path starts at source
    if (path.empty() || path.front() != source)
    {
        return {}; // No path exists
    }
    return path;
}

nlohmann::json computeShortestPaths(const Graph& graph)
{
    const int n = graph.num_nodes;
    nlohmann::json result = nlohmann::json::object();

    // Thread-local storage for Dijkstra results
    std::vector<nlohmann::json> thread_results(n);

#pragma omp parallel
    {
        hpc::ActiveThreadScope activeThreadScope;

#pragma omp for schedule(dynamic)
        for (int src = 0; src < n; ++src)
        {
            std::vector<double> dist;
            std::vector<int> prev;
            dijkstraSingleSource(graph, src, dist, prev);

            nlohmann::json local_result = nlohmann::json::object();

            for (int dst = 0; dst < n; ++dst)
            {
                if (src == dst)
                {
                    continue;
                }
                if (dist[dst] == std::numeric_limits<double>::infinity())
                {
                    continue; // No path
                }

                std::vector<int> path_indices = reconstructPath(prev, src, dst);
                if (path_indices.empty())
                {
                    continue;
                }

                // Convert indices to node names
                nlohmann::json path_names = nlohmann::json::array();
                for (int idx : path_indices)
                {
                    path_names.push_back(graph.node_names[idx]);
                }

                std::string key =
                    graph.node_names[src] + "-" + graph.node_names[dst];

                local_result[key] = {
                    {"path", path_names},
                    {"cost", dist[dst]}};
            }

            thread_results[src] = std::move(local_result);
        }
    }

    // Merge thread-local results
    for (int src = 0; src < n; ++src)
    {
        for (auto& [key, val] : thread_results[src].items())
        {
            result[key] = std::move(val);
        }
    }

    return result;
}

} // namespace hpc

/**
 * @file connected_components.cpp
 * @brief Connected component detection treating the graph as undirected.
 */

#include "hpc/connected_components.hpp"

#include "hpc/metrics.hpp"

#include <algorithm>
#include <queue>
#include <vector>

#include <omp.h>

namespace hpc
{

nlohmann::json computeConnectedComponents(const Graph& graph)
{
    const int n = graph.num_nodes;

    // Build undirected adjacency (add reverse edges)
    std::vector<std::vector<int>> undirected(n);

    for (int u = 0; u < n; ++u)
    {
        for (const auto& edge : graph.adjacency[u])
        {
            undirected[u].push_back(edge.to);
            undirected[edge.to].push_back(u);
        }
    }

    // BFS-based component labeling
    std::vector<int> component_id(n, -1);
    int num_components = 0;

    for (int start = 0; start < n; ++start)
    {
        if (component_id[start] != -1)
        {
            continue;
        }

        // BFS from this unvisited node
        std::queue<int> bfs_queue;
        bfs_queue.push(start);
        component_id[start] = num_components;

        while (!bfs_queue.empty())
        {
            int u = bfs_queue.front();
            bfs_queue.pop();

            for (int v : undirected[u])
            {
                if (component_id[v] == -1)
                {
                    component_id[v] = num_components;
                    bfs_queue.push(v);
                }
            }
        }

        ++num_components;
    }

    // Group nodes by component using thread-local buckets to avoid omp critical.
    // Each thread accumulates into its own local components array;
    // results are merged serially afterwards (reduction pattern for non-primitive types).
    int max_threads = omp_get_max_threads();
    std::vector<std::vector<std::vector<std::string>>> local_components(
        max_threads, std::vector<std::vector<std::string>>(num_components));

#pragma omp parallel
    {
        hpc::ActiveThreadScope activeThreadScope;

#pragma omp for schedule(static)
        for (int i = 0; i < n; ++i)
        {
            int tid = omp_get_thread_num();
            int cid = component_id[i];
            local_components[tid][cid].push_back(graph.node_names[i]);
        }
    }

    // Serial merge of thread-local buckets (reduction of non-primitive type)
    std::vector<std::vector<std::string>> components(num_components);
    for (int t = 0; t < max_threads; ++t)
    {
        for (int c = 0; c < num_components; ++c)
        {
            for (const auto& name : local_components[t][c])
            {
                components[c].push_back(name);
            }
        }
    }

    // Sort each component for deterministic output
    for (auto& comp : components)
    {
        std::sort(comp.begin(), comp.end());
    }

    // Build JSON result
    nlohmann::json result = nlohmann::json::array();
    for (const auto& comp : components)
    {
        result.push_back(comp);
    }

    return result;
}

} // namespace hpc

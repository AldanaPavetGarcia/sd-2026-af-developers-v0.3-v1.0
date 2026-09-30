/**
 * @file centrality.cpp
 * @brief Betweenness Centrality using Brandes' algorithm with OpenMP parallelization.
 *
 * Each source node runs an independent BFS + back-propagation.
 * Thread-local centrality accumulators avoid race conditions.
 * Final scores are normalized by (V-1)(V-2) for directed graphs.
 *
 * Results are exact Brandes betweenness. The work here is dominated by memory behaviour rather
 * than arithmetic — at 10K nodes a single source scans ~40K edges and records ~15K predecessor
 * entries, so the layout of those two structures decides the runtime (US-201 AC3). Both are
 * therefore flattened into contiguous buffers built once per call:
 *
 * - Adjacency is copied into CSR form, replacing the per-node heap blocks of
 *   `vector<vector<Edge>>` with two flat arrays, so the inner scan walks memory linearly.
 * - Predecessors are kept as an intrusive linked list inside one preallocated buffer instead of
 *   a `vector<vector<int>>`, which turned every recorded predecessor into a separate vector's
 *   bookkeeping (and potential reallocation) on a cold cache line.
 *
 * Per-source state is also reset by walking only the nodes that source reached, since wiping the
 * whole graph per source adds an O(V^2) term to what should be O(V*E).
 */

#include "hpc/centrality.hpp"

#include "hpc/metrics.hpp"

#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <omp.h>

namespace hpc
{

    namespace
    {
        /// Marks a node as not yet reached by the current source's BFS.
        constexpr int UNVISITED_DISTANCE = -1;

        /// Terminates a node's predecessor chain (see PredecessorList).
        constexpr int NO_PREDECESSOR = -1;

        /// Fixed seed for pivot selection: sampling must be reproducible run to run, so that the
        /// same graph always yields the same scores (ADR-019).
        constexpr unsigned int kCentralitySamplingSeed = 20260909U;

        /**
         * @brief Reads a positive integer from the environment, falling back on anything unusable.
         *
         * @param name Environment variable to read.
         * @param default_value Value returned when the variable is unset, empty, malformed, or
         *                      not strictly positive.
         * @return The configured value, or @p default_value. Never throws.
         */
        int readPositiveIntEnv(const char* name, int default_value)
        {
            const char* raw = std::getenv(name);
            if (raw == nullptr || *raw == '\0')
            {
                return default_value;
            }

            try
            {
                const int parsed = std::stoi(raw);
                return (parsed > 0) ? parsed : default_value;
            }
            catch (...)
            {
                return default_value;
            }
        }

        /**
         * @brief Chooses which source nodes to traverse.
         *
         * @details Below the exact threshold every node is a source, giving exact betweenness. Above
         * it, sources are a partial Fisher-Yates sample driven by a fixed seed: uniform over the
         * graph, yet identical on every run, so the same request always returns the same scores
         * (ADR-019).
         *
         * @param n Node count of the graph.
         * @param[out] approximate Set to true when the returned set is a sample rather than all nodes.
         * @return Source node indices to run Brandes from.
         */
        std::vector<int> selectSources(int n, bool& approximate)
        {
            const int exact_max_nodes =
                readPositiveIntEnv("EOP_HPC_CENTRALITY_EXACT_MAX_NODES", kDefaultCentralityExactMaxNodes);

            if (n <= exact_max_nodes)
            {
                approximate = false;
                std::vector<int> sources(static_cast<std::size_t>(n));
                for (int i = 0; i < n; ++i)
                {
                    sources[static_cast<std::size_t>(i)] = i;
                }
                return sources;
            }

            int sample_count = readPositiveIntEnv("EOP_HPC_CENTRALITY_SAMPLES", kDefaultCentralitySampleCount);
            if (sample_count >= n)
            {
                // Sampling every node is just the exact algorithm; report it as such.
                approximate = false;
                std::vector<int> sources(static_cast<std::size_t>(n));
                for (int i = 0; i < n; ++i)
                {
                    sources[static_cast<std::size_t>(i)] = i;
                }
                return sources;
            }

            approximate = true;

            // Partial Fisher-Yates over the node range, driven by a fixed-seed generator: the sample
            // is uniform over the graph yet identical on every run, so the same request always
            // returns the same scores. A fixed stride would also be reproducible but would sample
            // periodically, which biases any graph whose node ordering carries periodic structure.
            std::vector<int> candidates(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i)
            {
                candidates[static_cast<std::size_t>(i)] = i;
            }

            std::mt19937 generator(kCentralitySamplingSeed);
            for (int k = 0; k < sample_count; ++k)
            {
                std::uniform_int_distribution<int> pick(k, n - 1);
                std::swap(candidates[static_cast<std::size_t>(k)],
                          candidates[static_cast<std::size_t>(pick(generator))]);
            }

            candidates.resize(static_cast<std::size_t>(sample_count));
            return candidates;
        }

        /**
         * @brief Read-only CSR view of the graph's adjacency, built once per computeCentrality call.
         *
         * @details Brandes visits every edge once per source, so with V sources the adjacency is
         * traversed V times. Holding it as `vector<vector<Edge>>` means each of those traversals
         * chases V separate heap allocations; CSR collapses that to two sequential arrays.
         * Edge weights are omitted deliberately: this implementation measures betweenness over
         * hop-count shortest paths (BFS), so weights play no part in it.
         */
        struct CsrAdjacency
        {
            std::vector<int> offsets; ///< offsets[v]..offsets[v+1] bound node v's outgoing edges.
            std::vector<int> targets; ///< Flattened target node index of every outgoing edge.
        };

        CsrAdjacency buildCsrAdjacency(const Graph& graph)
        {
            const int n = graph.num_nodes;

            CsrAdjacency csr;
            csr.offsets.resize(static_cast<std::size_t>(n) + 1, 0);

            std::size_t total_edges = 0;
            for (int v = 0; v < n; ++v)
            {
                total_edges += graph.adjacency[v].size();
                csr.offsets[static_cast<std::size_t>(v) + 1] = static_cast<int>(total_edges);
            }

            csr.targets.resize(total_edges);
            std::size_t cursor = 0;
            for (int v = 0; v < n; ++v)
            {
                for (const auto& edge : graph.adjacency[v])
                {
                    csr.targets[cursor++] = edge.to;
                }
            }

            return csr;
        }

        /**
         * @brief Predecessor sets for one BFS, stored as intrusive lists in one flat buffer.
         *
         * @details Every recorded predecessor consumes one slot of `nodes`, chained through `next`;
         * `head[w]` starts node w's chain. A source can record at most one predecessor per edge, so
         * a buffer of edge-count slots never needs to grow — the whole structure is allocated once
         * per thread and reused for every source that thread handles.
         */
        struct PredecessorList
        {
            std::vector<int> head;  ///< head[w] = first slot of w's chain, or NO_PREDECESSOR.
            std::vector<int> nodes; ///< nodes[i] = the predecessor node recorded in slot i.
            std::vector<int> next;  ///< next[i] = following slot in the chain, or NO_PREDECESSOR.
            std::size_t used = 0;   ///< Slots consumed by the source currently being processed.

            PredecessorList(int node_count, std::size_t edge_count)
                : head(static_cast<std::size_t>(node_count), NO_PREDECESSOR)
                , nodes(edge_count)
                , next(edge_count)
            {
            }

            void add(int w, int v)
            {
                const std::size_t slot = used++;
                nodes[slot] = v;
                next[slot] = head[static_cast<std::size_t>(w)];
                head[static_cast<std::size_t>(w)] = static_cast<int>(slot);
            }
        };
    } // namespace

    nlohmann::json computeCentrality(const Graph& graph)
    {
        CentralityStats ignored;
        return computeCentrality(graph, ignored);
    }

    nlohmann::json computeCentrality(const Graph& graph, CentralityStats& stats)
    {
        const int n = graph.num_nodes;

        bool approximate = false;
        const std::vector<int> sources = selectSources(n, approximate);
        const int source_count = static_cast<int>(sources.size());

        stats.approximate = approximate;
        stats.sampled_sources = source_count;
        stats.total_sources = n;

        // Global centrality scores (accumulated from all threads)
        std::vector<double> centrality(n, 0.0);

        const CsrAdjacency csr = buildCsrAdjacency(graph);
        const std::size_t edge_count = csr.targets.size();

        // Determine number of threads for thread-local storage
        int max_threads = omp_get_max_threads();
        std::vector<std::vector<double>> thread_centrality(max_threads, std::vector<double>(n, 0.0));

#pragma omp parallel
        {
            hpc::ActiveThreadScope activeThreadScope;

            int tid = omp_get_thread_num();

            // Per-thread Brandes state, allocated once and reused across every source this thread
            // handles, so the source loop performs no allocation at all.
            PredecessorList predecessors(n, edge_count);
            std::vector<double> sigma(n, 0.0);
            std::vector<double> delta(n, 0.0);
            std::vector<int> dist(n, UNVISITED_DISTANCE);

            // BFS visit order, which doubles as the back-propagation stack (traversed in reverse) and
            // as the exact list of nodes to reset afterwards.
            std::vector<int> visit_order;
            visit_order.reserve(n);

#pragma omp for schedule(dynamic)
            for (int source_index = 0; source_index < source_count; ++source_index)
            {
                const int s = sources[static_cast<std::size_t>(source_index)];

                visit_order.clear();
                predecessors.used = 0;

                sigma[s] = 1.0;
                dist[s] = 0;
                visit_order.push_back(s);

                // BFS phase: discover shortest paths. visit_order is used as the queue itself, with
                // head walking forward over it — nodes are appended in the order they are dequeued,
                // which is exactly the order back-propagation needs to walk in reverse.
                for (std::size_t queue_head = 0; queue_head < visit_order.size(); ++queue_head)
                {
                    const int v = visit_order[queue_head];
                    const int next_distance = dist[v] + 1;
                    const double sigma_v = sigma[v];

                    const std::size_t begin = static_cast<std::size_t>(csr.offsets[static_cast<std::size_t>(v)]);
                    const std::size_t end = static_cast<std::size_t>(csr.offsets[static_cast<std::size_t>(v) + 1]);

                    for (std::size_t e = begin; e < end; ++e)
                    {
                        const int w = csr.targets[e];

                        // First visit to w?
                        if (dist[w] == UNVISITED_DISTANCE)
                        {
                            dist[w] = next_distance;
                            visit_order.push_back(w);
                        }

                        // Shortest path to w via v?
                        if (dist[w] == next_distance)
                        {
                            sigma[w] += sigma_v;
                            predecessors.add(w, v);
                        }
                    }
                }

                // Back-propagation phase: accumulate pair dependencies, walking the BFS order in
                // reverse (deepest first), which is what Brandes' dependency accumulation requires.
                for (std::size_t i = visit_order.size(); i-- > 0;)
                {
                    const int w = visit_order[i];
                    const double coefficient = (1.0 + delta[w]) / sigma[w];

                    for (int slot = predecessors.head[static_cast<std::size_t>(w)]; slot != NO_PREDECESSOR;
                         slot = predecessors.next[static_cast<std::size_t>(slot)])
                    {
                        const int v = predecessors.nodes[static_cast<std::size_t>(slot)];
                        delta[v] += sigma[v] * coefficient;
                    }

                    if (w != s)
                    {
                        thread_centrality[tid][w] += delta[w];
                    }
                }

                // Reset only what this source touched. Nodes it never reached still hold their
                // initial values, so wiping the whole graph here would be redundant work.
                for (const int v : visit_order)
                {
                    predecessors.head[static_cast<std::size_t>(v)] = NO_PREDECESSOR;
                    sigma[v] = 0.0;
                    delta[v] = 0.0;
                    dist[v] = UNVISITED_DISTANCE;
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

        // Sampling traversed only a fraction of the sources, so the accumulated dependencies
        // represent that fraction of the total; scaling by V/k puts the estimate back on the same
        // scale as an exact run (Brandes & Pich). Exact runs have source_count == n, leaving this a
        // no-op multiplication by 1.
        const double sampling_scale =
            (source_count > 0) ? (static_cast<double>(n) / static_cast<double>(source_count)) : 1.0;

        nlohmann::json result = nlohmann::json::object();
        for (int i = 0; i < n; ++i)
        {
            result[graph.node_names[i]] = (centrality[i] * sampling_scale) / normalization;
        }

        return result;
    }

} // namespace hpc

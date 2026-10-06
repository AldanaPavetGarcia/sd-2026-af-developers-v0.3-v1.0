# ADR-024: Approximate Betweenness Centrality for Large Graphs

**Status:** Accepted for graphs up to 10,000 nodes (option D below): US-201 AC3 is met with an
**exact** computation at the size it names, so nothing is approximated there. Above 10,000 nodes,
pivot sampling changes *what is computed* to keep large-graph requests interactive, which the
course staff has not yet reviewed — see [Open Decision](#open-decision).

> **Corrections.** Two measurement errors are corrected in this version; every figure below was
> re-measured on 2026-09-19 with the method under
> [Reproducing the Measurements](#reproducing-the-measurements).
>
> 1. *Unoptimized build (2026-09-18).* The first version quoted 39.6 s for the 10K-node case and
>    15.3 s → 10.3 s for the exact-path optimizations. Those came from the development preset, which
>    compiles with `-O0` and overstated the cost about 4.6x.
> 2. *Power state (2026-09-19).* The re-measurement of 2026-09-18 was optimized but taken without
>    recording the power state, very likely on battery: the same binary on the same graph ran
>    2.5–3x faster the next day, plugged in, with every configuration moving by the same factor.
>    All timings are now taken plugged in, and the power state is recorded with them.
>
> The corrections move the absolute numbers, not the conclusions about accuracy, which does not
> depend on either. They do change the argument: at 10K nodes the exact algorithm already meets
> US-201 AC3 on this machine, so sampling is justified by graphs larger than the one the AC names.

## Context and Problem

**US-201 AC3** requires a 10,000-node graph analyzed with 4 threads to return to the client in
under 5 seconds.

All figures are from an Intel i7-8550U laptop (4 physical cores, 8 hardware threads), optimized
build, plugged in. At 10K nodes the implementation as it stood spent 2.97 s in `centrality` and
24 ms in `connected_components`: the cost is betweenness centrality.

Betweenness centrality is computed with Brandes' algorithm, which runs one BFS plus a dependency
back-propagation from **every** node as source. That is O(V·E): at V=10,000 and E≈40,000 it means
roughly 400 million edge traversals and about 147 million predecessor records per full run.

Parallelization is not the problem. The original implementation scales well up to the physical
core count:

| Threads | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| Time | 6.77 s | 3.72 s | 2.97 s | 2.71 s |
| Speedup | 1.00x | 1.82x | 2.28x | 2.50x |

The cost that matters is how the algorithm grows with graph size. In the US-202 benchmark on the
same laptop, the original exact implementation with 4 threads took **165 s at 50,000 nodes and
1,109 s — about 18.5 minutes — at 100,000 nodes** (on a second machine, an i7-10510U: 213 s and
1,490 s). The product targets topologies of that size, and for an interactive request, minutes are
not a latency, they are an outage.

Two causes therefore had to be separated: implementation overhead that could be removed without
changing results, and the algorithmic cost that could not.

## Decision

**Both**: first remove the avoidable constant factors, then sample sources above a configured
graph size.

### 1. Exact-path optimizations (results unchanged)

The runtime is dominated by memory behaviour, not arithmetic. Three changes:

- **CSR adjacency.** `vector<vector<Edge>>` makes every one of the V traversals chase V separate
  heap blocks. The adjacency is copied once per call into two flat arrays so the inner scan walks
  memory linearly.
- **Flat predecessor storage.** Predecessors were `vector<vector<int>>`, turning each of the ~147
  million records into bookkeeping on a separate vector, on a cold cache line, with possible
  reallocation. They are now an intrusive linked list inside a single preallocated buffer, sized
  once per thread from the edge count (a source can record at most one predecessor per edge).
- **Touched-only reset.** Per-source state was cleared by wiping all V entries, adding an O(V²)
  term to an O(V·E) algorithm. Only the nodes a source actually reached are reset, which the BFS
  visit order already enumerates.

Measured at 10K nodes: **2.97 s → 2.11 s** with 4 threads (1.41x), and 6.77 s → 5.43 s with 1.

The results are the same exact Brandes values, **equal up to floating-point rounding**: the largest
difference from the original implementation is 1.7·10⁻¹⁸ against a largest score of 1.1·10⁻², and
the ranking of all 10,000 nodes is identical. Bit-for-bit equality was never a property of this
code — the original implementation already varies by up to 3.5·10⁻¹⁷ between thread counts, and
between two runs at 8 threads, because dynamic scheduling changes the order in which per-thread
partial sums are added.

An additional attempt — interleaving `dist` and `sigma` into one array to halve cache misses —
made it slower and was discarded. That comparison was only ever measured on the unoptimized
build, and the code no longer exists to re-measure it.

### 2. Pivot sampling above a size threshold

With the optimizations, exact centrality at 10K nodes returns to the client in **3.42 s end to
end** — client → `eop-server` → `hpc-engine` → client, median of 3 — inside the 5 s budget with
about 30% margin. **At the size US-201 AC3 names, sampling is not needed on this machine.** The
cost keeps growing as O(V·E) past 10K, though, as the 18.5-minute figure at 100K nodes shows, and
that is what sampling is for.

Above a configured node count, betweenness is therefore **estimated** from a sample of source nodes
and rescaled by V/k (Brandes & Pich), which bounds the cost to O(k·E):

- `EOP_HPC_CENTRALITY_EXACT_MAX_NODES` (default 10000, option D) — at or below this, every node
  is a source and the result is exact. Set to cover US-201 AC3's own graph size.
- `EOP_HPC_CENTRALITY_SAMPLES` (default 1024) — sources sampled above that threshold.

With 4 threads and the default 1024 samples, centrality takes **0.20 s** at 10K nodes (1.90 s end
to end, most of it moving ~1 MB of JSON), 1.11 s at 50K and 4.90 s at 100K.

Pivots are selected by a partial Fisher-Yates shuffle driven by a **fixed seed**, so the same
graph always produces the same scores; a request is reproducible, which both the test suite and a
live demo depend on. A fixed stride was implemented first and rejected: it is equally
reproducible but samples periodically, which would bias any graph whose node ordering carries
periodic structure.

### 3. The estimate is declared in the response

When sampling is used, the response carries an additive, optional `result.algorithm_metadata`
field:

```json
"algorithm_metadata": {
  "centrality": {
    "approximate": true,
    "method": "brandes_pivot_sampling",
    "sampled_sources": 1024,
    "total_sources": 10000
  }
}
```

The field is omitted entirely when there is nothing to declare, so responses that were exact keep
exactly the shape they had before. This follows the additive extension approach ADR-015 already
established for this protocol.

## Measured Accuracy and Latency

10,000 nodes / 39,995 edges, 4 threads, i7-8550U plugged in, optimized build, median of 3 runs. Accuracy is
compared against the exact run on the same graph. "top-N" is the share of the exact top-N
most-central nodes that the estimate also places in its top N. Mean relative error is
|estimate − exact| / exact, averaged over the 9,906 nodes whose exact score is non-zero. "Engine
process" is the engine alone from request in to response out — parsing, computation and
serialization — without `eop-server` transport.

| Samples | Centrality | Engine process | top-10 | top-50 | top-100 | Mean relative error |
|---:|---:|---:|---:|---:|---:|---:|
| exact (10000) | 2.11 s | 2.28 s | 100% | 100% | 100% | 0% |
| 2048 | 0.41 s | 0.58 s | 90% | 86% | 82% | 21.9% |
| **1024 (default)** | **0.20 s** | **0.35 s** | **90%** | **80%** | **73%** | **31.1%** |
| 512 | 0.09 s | 0.26 s | 90% | 66% | 61% | 41.1% |
| 256 | 0.05 s | 0.19 s | 80% | 54% | 48% | 50.7% |
| 128 | 0.02 s | 0.16 s | 50% | 32% | 38% | 62.9% |

About 0.15 s of every engine figure is parsing and serializing the 10K-node request, which is why
the column stops shrinking below 256 samples. Through `eop-server`, end to end adds about 1.2–1.5 s
of transfer: 1.90 s sampled and 3.42 s exact.

**Accuracy varies between graphs of the same shape.** An earlier measurement on a different random
10K-node graph gave top-10 = 100% at 2048 samples and top-100 = 82% at 1024. Treat the table as
indicative to within about ten points, not as a guarantee for any particular topology.

## Decision Made, and What Is Still Open

**Option D is implemented and is the default** (`EOP_HPC_CENTRALITY_EXACT_MAX_NODES=10000`,
`hpc-engine/include/hpc/centrality.hpp`). With the corrected measurements the options were:

| Option | US-201 AC3 (10K, end to end) | 100K nodes | What changes |
|---|---:|---:|---|
| A — sample above 2,000, 1024 samples | 1.90 s, approximate | 4.9 s | ~2.6x margin; 31% mean error already at 10K. |
| B — as A, 2048 samples | ~2.1 s (est.), approximate | ~10 s (est.) | Error drops to 22%; top-50/top-100 improve. Both times estimated from A by the sample count, not measured. |
| C — exact only | 3.42 s, exact | ~18.5 min (original impl.; est. ~13 min optimized) | Meets AC3; unusable at the sizes US-202 benchmarks. |
| **D — exact up to 10,000, sample above** (**implemented**) | 3.42 s, **exact** | 4.9 s | `EOP_HPC_CENTRALITY_EXACT_MAX_NODES=10000`. |

**D was chosen** because it meets US-201 AC3 without approximating anything at the size the AC
names — the AC talks about 10,000 nodes, not 100,000, and at 10,000 nodes exact computation
already fits the 5 s budget. That narrows what is actually left open for the course staff: only
graphs larger than any AC's latency figure names, where the choice is between an estimate in
seconds and an exact answer in tens of minutes. D's cost is a smaller margin at 10K (about 30%
instead of 2.6x), which shrinks on slower hardware: on battery the same exact request took about
3x longer. A, B and D differ only in the value of `EOP_HPC_CENTRALITY_EXACT_MAX_NODES` /
`EOP_HPC_CENTRALITY_SAMPLES`, so switching between them needs no code change if the course staff's
answer calls for it.

**Still open:** whether pivot-sampled (approximate) centrality is acceptable at all for graphs
above 10,000 nodes. US-202 exercises 50,000 and 100,000-node graphs, where exact computation costs
minutes per request (see the Alternatives table below) — that is what the sampling above the
threshold is for, and it is what remains unreviewed.

## Alternatives Considered

| Alternative | Reason for Rejection |
| --- | --- |
| **Exact Brandes only, optimized** | Meets AC3 at 10K nodes (3.42 s end to end, plugged in), but its O(V·E) cost reaches about 18.5 minutes per request at 100K nodes. It remains the mode used at or below the threshold, and any deployment can force it by raising `EOP_HPC_CENTRALITY_EXACT_MAX_NODES`. |
| **Throw more cores at it** | The AC fixes the thread count at 4. At 50K and 100K nodes the exact algorithm is memory-bound: in the US-202 benchmark, 8 threads on 4 cores were slower than 4 at both sizes, and 4 threads reached only 1.86x and 1.60x. More threads competing for the same cache do not make it faster. |
| **Reject centrality requests above a size limit** | Honest, and cheap to implement, but it removes the capability precisely for the large topologies where identifying critical bunkers matters most — the opposite of what US-201 asks for. |
| **Approximate silently, without response metadata** | Would meet the latency AC while making an estimate indistinguishable from an exact figure. An operator deciding which bunker is critical would have no way to know the number carries a 31% mean error. |
| **Random (unseeded) pivots** | Slightly better expected accuracy per sample, but the same request would return different scores on every run, breaking reproducible tests and making a live demo unrepeatable. |

## Justification (Trade-off Analysis)

* **Large graphs stay interactive**: 4.9 s of centrality at 100K nodes instead of about 18.5
  minutes, which is the case exact cannot serve.
* **Margin that does not hinge on the machine**: at 10K nodes sampling answers in 1.90 s end to
  end, where exact takes 3.42 s plugged in and about three times that on battery.
* **US-201 AC3's own size is unaffected**: at or below 10,000 nodes nothing is approximated —
  same algorithm, exact results, and every existing test continues to assert exact values.
* **The trade is visible**: the response says when a number is an estimate and how it was
  produced, so the accuracy limits documented above travel with the data.
* **Reproducible**: a fixed seed means identical scores for identical input, which keeps the
  behaviour testable and demonstrable.
* **Tunable at deploy time**: both the threshold and the sample count are configuration, so a
  deployment with a different latency budget can move along the accuracy curve above without a
  code change.

## Consequences

* **Positive**: US-201 AC3 is met at 10K nodes **exactly**, approximating nothing; the
  exact-path optimizations also make exact centrality about 1.4x faster at 10K nodes, which
  benefits every graph still computed exactly (up to the 10,000-node threshold).

* **Drawbacks**: above 10,000 nodes, centrality scores carry meaningful error — around 31% mean
  relative error at the default sample count, and roughly one in ten of the top-10 most central
  nodes may be misidentified. Absolute score values should not be compared across runs with
  different sample counts, and should not be presented as exact measurements. This is the part of
  the decision still pending course-staff review (see Open Decision).

* **Restrictions Introduced**:
  * Any consumer that treats `centrality` values as exact must check
    `result.algorithm_metadata.centrality.approximate` first.
  * Changing `EOP_HPC_CENTRALITY_SAMPLES` changes the scores, not just the runtime. Comparing
    centrality across time is only meaningful when the sample count is held constant.
  * The accuracy table was measured on one graph shape (≈4 edges per node), and varies between
    random graphs of that same shape. Substantially denser or more clustered topologies should be
    re-measured before the defaults are relied on for them.
  * Performance figures for this engine must come from an optimized build, plugged in, with the
    power state recorded. The development preset (`build/dev`) compiles with `-O0` and overstates
    centrality's cost about 4.6x; running on battery overstated it another 2.5–3x. Both errors are
    described in this ADR's correction note.

## Reproducing the Measurements

The graph is generated with the benchmark's own generator and a fixed seed:

```bash
python3 -c "import random, sys, json; sys.path.insert(0, 'hpc-engine/benchmarks'); \
import generate_graph as g; random.seed(20260918); \
json.dump(g.generate_graph(10000, 2.0), open('g10k.json', 'w'))"
```

The engine must be built with optimization (`-O3 -DNDEBUG`). Exact and sampled runs:

```bash
OMP_NUM_THREADS=4 EOP_HPC_CENTRALITY_EXACT_MAX_NODES=200000 ./hpc_engine < g10k.json   # exact
OMP_NUM_THREADS=4 EOP_HPC_CENTRALITY_SAMPLES=1024 ./hpc_engine < g10k.json             # sampled
```

Timings are the median of 3 runs of `result.processing_time_ms` and
`result.algorithm_timings_ms.centrality`, taken plugged in; "engine process" is the wall-clock
time of the process. The end-to-end figures through `eop-server` come from
`tests/evidence/us201_ac3_e2e.py`. The 50K and 100K figures use the same generator and seed with
density 0.5, as the US-202 benchmark script does for those sizes.

## References

* Brandes, U. (2001), *A Faster Algorithm for Betweenness Centrality* — the exact algorithm.
* Brandes, U. & Pich, C. (2007), *Centrality Estimation in Large Networks* — the pivot-sampling
  estimator and its V/k rescaling.
* `hpc-engine/src/centrality.cpp` — `selectSources`, `CsrAdjacency`, `PredecessorList`.
* `hpc-engine/include/hpc/centrality.hpp` — `CentralityStats`, default threshold and sample count.
* `docs/analyze-graph-protocol-v0.2.md` — the `algorithm_metadata` response field.
* `ADR-015-protocol-extension-and-trace-propagation.md` — the additive extension approach this field follows.

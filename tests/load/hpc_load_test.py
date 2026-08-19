#!/usr/bin/env python3
"""
Local load test for hpc-engine performance telemetry.

It runs many ANALYZE_GRAPH requests against the hpc_engine binary and validates
that each ACK response contains the telemetry fields required by US-206.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


def build_request(request_id: str, nodes_count: int) -> dict[str, Any]:
    nodes = [f"N{i}" for i in range(nodes_count)]

    edges: list[dict[str, Any]] = []
    for i in range(nodes_count - 1):
        edges.append({"from": f"N{i}", "to": f"N{i + 1}", "weight": 1.0})

    # Add a few extra edges to avoid an overly trivial graph.
    for i in range(nodes_count - 2):
        edges.append({"from": f"N{i}", "to": f"N{i + 2}", "weight": 2.0})

    return {
        "type": "ANALYZE_GRAPH",
        "request_id": request_id,
        "payload": {
            "nodes": nodes,
            "edges": edges,
            "algorithms": [
                "shortest_paths",
                "connected_components",
                "centrality",
            ],
        },
    }


def require_number(value: Any, field: str) -> None:
    if not isinstance(value, (int, float)):
        raise AssertionError(f"{field} must be numeric, got {type(value).__name__}")
    if value < 0:
        raise AssertionError(f"{field} must be non-negative, got {value}")


def validate_response(response: dict[str, Any], expected_request_id: str) -> None:
    if response.get("type") != "RESPONSE":
        raise AssertionError(f"unexpected response type: {response.get('type')}")
    if response.get("status") != "ACK":
        raise AssertionError(f"unexpected response status: {response.get('status')}")
    if response.get("request_id") != expected_request_id:
        raise AssertionError(
            f"request_id mismatch: expected {expected_request_id}, got {response.get('request_id')}"
        )

    result = response.get("result")
    if not isinstance(result, dict):
        raise AssertionError("result must be an object")

    graph_size = result.get("graph_size")
    if not isinstance(graph_size, dict):
        raise AssertionError("result.graph_size must be an object")

    require_number(graph_size.get("nodes"), "result.graph_size.nodes")
    require_number(graph_size.get("edges"), "result.graph_size.edges")
    require_number(result.get("processing_time_ms"), "result.processing_time_ms")
    require_number(result.get("thread_count"), "result.thread_count")

    executed_algorithms = result.get("executed_algorithms")
    if not isinstance(executed_algorithms, list) or not executed_algorithms:
        raise AssertionError("result.executed_algorithms must be a non-empty array")

    algorithm_timings = result.get("algorithm_timings_ms")
    if not isinstance(algorithm_timings, dict):
        raise AssertionError("result.algorithm_timings_ms must be an object")

    for algorithm in executed_algorithms:
        if algorithm not in algorithm_timings:
            raise AssertionError(
                f"missing timing for executed algorithm '{algorithm}'"
            )
        require_number(
            algorithm_timings[algorithm],
            f"result.algorithm_timings_ms.{algorithm}",
        )

    algorithms = result.get("algorithms")
    if not isinstance(algorithms, dict):
        raise AssertionError("result.algorithms must be an object")

    for algorithm in executed_algorithms:
        if algorithm not in algorithms:
            raise AssertionError(f"missing result for executed algorithm '{algorithm}'")


def run_one(engine: Path, index: int, nodes: int, timeout_seconds: int) -> float:
    request_id = f"load-hpc-{index:04d}"
    payload = json.dumps(build_request(request_id, nodes), separators=(",", ":"))

    started = time.perf_counter()
    completed = subprocess.run(
        [str(engine)],
        input=payload,
        text=True,
        capture_output=True,
        timeout=timeout_seconds,
        check=False,
    )
    elapsed_ms = (time.perf_counter() - started) * 1000.0

    if completed.returncode != 0:
        raise RuntimeError(
            f"hpc_engine returned {completed.returncode} for {request_id}\n"
            f"stdout={completed.stdout}\nstderr={completed.stderr}"
        )

    try:
        response = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"invalid JSON response for {request_id}: {exc}\nstdout={completed.stdout}"
        ) from exc

    validate_response(response, request_id)
    return elapsed_ms


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--engine",
        default="./build/hpc-engine/hpc_engine",
        help="Path to the hpc_engine binary",
    )
    parser.add_argument("--requests", type=int, default=30)
    parser.add_argument("--concurrency", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--nodes", type=int, default=12)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    args = parser.parse_args()

    engine = Path(args.engine)
    if not engine.exists():
        print(f"ERROR: hpc_engine binary not found: {engine}", file=sys.stderr)
        print("Build first with: cmake --build build -j\"$(nproc)\"", file=sys.stderr)
        return 2

    if args.requests <= 0:
        print("ERROR: --requests must be greater than 0", file=sys.stderr)
        return 2

    if args.concurrency <= 0:
        print("ERROR: --concurrency must be greater than 0", file=sys.stderr)
        return 2

    print(
        "[load-test-hpc] starting "
        f"requests={args.requests} concurrency={args.concurrency} nodes={args.nodes}"
    )

    failures: list[str] = []
    latencies_ms: list[float] = []

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = [
            executor.submit(run_one, engine, i, args.nodes, args.timeout_seconds)
            for i in range(args.requests)
        ]

        for future in concurrent.futures.as_completed(futures):
            try:
                latencies_ms.append(future.result())
            except Exception as exc:  # noqa: BLE001 - test runner must collect all failures
                failures.append(str(exc))

    if failures:
        print(f"[load-test-hpc] failures={len(failures)}", file=sys.stderr)
        for failure in failures[:10]:
            print(f"--- failure ---\n{failure}", file=sys.stderr)
        return 1

    latencies_ms.sort()
    avg = sum(latencies_ms) / len(latencies_ms)
    p50 = latencies_ms[int(0.50 * (len(latencies_ms) - 1))]
    p95 = latencies_ms[int(0.95 * (len(latencies_ms) - 1))]
    p99 = latencies_ms[int(0.99 * (len(latencies_ms) - 1))]

    print(
        "[load-test-hpc] passed "
        f"requests={len(latencies_ms)} "
        f"avg_ms={avg:.2f} "
        f"p50_ms={p50:.2f} "
        f"p95_ms={p95:.2f} "
        f"p99_ms={p99:.2f}"
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

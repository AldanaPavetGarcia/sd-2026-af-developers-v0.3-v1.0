#!/usr/bin/env python3
"""
US-201 AC3: "For graphs up to 10K nodes, results are returned to the requesting client in < 5
seconds with 4 threads."

Mide lo que dice el AC, de punta a punta: un cliente TCP manda ANALYZE_GRAPH a un eop_server real,
que lo despacha a un hpc_engine real en modo socket, y se cronometra desde que sale el primer byte
del pedido hasta que llega la última línea de la respuesta. El engine usa OMP_NUM_THREADS=4.

El grafo sale del generador del benchmark con semilla fija, así que es el mismo grafo que usan
el ADR-024 y el Benchmark Report. Las variables EOP_HPC_CENTRALITY_* del entorno se pasan al
engine, para medir la centralidad exacta o la muestreada.

Uso: us201_ac3_e2e.py <eop_server> <hpc_engine> [--nodes 10000] [--repeats 3]
"""
import argparse
import json
import os
import random
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "hpc-engine", "benchmarks"))
import generate_graph  # noqa: E402

AC3_THREADS = 4
AC3_BUDGET_S = 5.0
GRAPH_SEED = 20260918
GRAPH_DENSITY = 2.0
ENGINE_TIMEOUT_MS = 60_000  # que el timeout del server no corte antes de poder medir


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def recv_line(sock, timeout):
    sock.settimeout(timeout)
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = sock.recv(1 << 16)
        if not chunk:
            break
        data += chunk
    return bytes(data)


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(0.5)
    try:
        banner = s.recv(256)  # WELCOME
        if banner and not banner.startswith(b"WELCOME"):
            raise RuntimeError(f"banner inesperado: {banner[:80]!r}")
    except socket.timeout:
        pass
    return s


def wait_engine_registered(port, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with connect(port) as s:
                s.sendall(b"LIST_NODES\n")
                s.settimeout(1)
                data = b""
                while b"END_LIST" not in data:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                if b"node_id=hpc-engine" in data and b"status=ONLINE" in data:
                    return True
        except OSError:
            pass
        time.sleep(0.3)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("server")
    parser.add_argument("engine")
    parser.add_argument("--nodes", type=int, default=10_000)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()

    random.seed(GRAPH_SEED)
    graph = generate_graph.generate_graph(args.nodes, GRAPH_DENSITY)
    edges = len(graph["payload"]["edges"])

    port = free_port()
    workdir = tempfile.mkdtemp(prefix="eop-us201-ac3-")
    env = dict(os.environ, EOP_PORT=str(port), EOP_TRACING_ENABLED="0", EOP_HPC_TIMEOUT_MS=str(ENGINE_TIMEOUT_MS))
    server = subprocess.Popen([os.path.abspath(args.server)], cwd=workdir, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    engine = None
    try:
        time.sleep(1)
        engine_env = dict(os.environ, OMP_NUM_THREADS=str(AC3_THREADS), EOP_TRACING_ENABLED="0")
        engine = subprocess.Popen([os.path.abspath(args.engine), "--server-host", "127.0.0.1", "--server-port",
                                   str(port), "--node-id", "hpc-engine"], cwd=workdir, env=engine_env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_engine_registered(port):
            print("ERROR: el engine no se registró en el server")
            return 1

        mode = {k: v for k, v in os.environ.items() if k.startswith("EOP_HPC_CENTRALITY_")}
        print(f"grafo: {args.nodes} nodos / {edges} aristas (semilla {GRAPH_SEED}), {AC3_THREADS} hilos, "
              f"algoritmos {graph['payload']['algorithms']}, config de centralidad: {mode or 'por defecto'}")

        times, meta = [], None
        for i in range(args.repeats):
            graph["request_id"] = f"us201-ac3-{i}"
            payload = (json.dumps(graph, separators=(",", ":")) + "\n").encode()
            with connect(port) as s:
                start = time.perf_counter()
                s.sendall(payload)
                raw = recv_line(s, timeout=ENGINE_TIMEOUT_MS / 1000 + 10)
                elapsed = time.perf_counter() - start
            try:
                response = json.loads(raw)
            except ValueError:
                print(f"ERROR: respuesta no es JSON ({len(raw)} bytes): {raw[:200]!r}")
                return 1
            if response.get("status") != "ACK" or response.get("request_id") != graph["request_id"]:
                print(f"ERROR: respuesta inesperada: {str(response)[:300]}")
                return 1
            result = response["result"]
            meta = result.get("algorithm_metadata", {}).get("centrality")
            times.append(elapsed)
            print(f"   corrida {i + 1}: {elapsed:.3f} s de punta a punta  "
                  f"(engine: {result['processing_time_ms'] / 1000:.3f} s, {result['thread_count']} hilos)", flush=True)

        median = statistics.median(times)
        centrality = (f"muestreada ({meta['sampled_sources']} de {meta['total_sources']})"
                      if meta and meta.get("approximate") else "exacta")
        print(f"RESULTADO: mediana {median:.3f} s (min {min(times):.3f}, max {max(times):.3f}), "
              f"centralidad {centrality}, límite {AC3_BUDGET_S:.0f} s")
        print("US-201 AC3:", "CUMPLE" if median < AC3_BUDGET_S else "NO CUMPLE")
        return 0 if median < AC3_BUDGET_S else 1
    finally:
        for p in (engine, server):
            if p is not None and p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

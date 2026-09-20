#!/usr/bin/env python3
"""
generate_graph.py — Generador de grafos para el benchmark del HPC Engine.

Modelo: Erdős–Rényi esparso (árbol aleatorio + aristas extra controladas por --density).
Para grafos de >= 10000 nodos, shortest_paths se excluye automáticamente
para evitar el costo O(N² log N) de all-pairs Dijkstra.
"""
import json
import random
import sys
import argparse


LARGE_GRAPH_THRESHOLD = 500


def generate_graph(num_nodes: int, density: float) -> dict:
    """
    Genera un grafo conexo con `num_nodes` nodos.

    Parámetros
    ----------
    num_nodes : int
        Número de nodos del grafo.
    density : float
        Factor de densidad: número de aristas extra = int(num_nodes * density).
        0.0 → árbol puro.  2.0 → ~2× nodos de aristas extra (por defecto).
    """
    nodes = [f"N{i}" for i in range(num_nodes)]
    edges = []

    # Árbol de expansión aleatorio para garantizar conexidad
    for i in range(1, num_nodes):
        parent = random.randint(0, i - 1)
        w = round(random.uniform(1.0, 10.0), 2)
        edges.append({"from": nodes[parent], "to": nodes[i], "weight": w})
        edges.append({"from": nodes[i], "to": nodes[parent], "weight": round(random.uniform(1.0, 10.0), 2)})

    # Aristas extra (Erdős–Rényi esparso)
    extra_count = int(num_nodes * density)
    for _ in range(extra_count):
        u = random.randint(0, num_nodes - 1)
        v = random.randint(0, num_nodes - 1)
        if u != v:
            edges.append({
                "from": nodes[u],
                "to": nodes[v],
                "weight": round(random.uniform(1.0, 10.0), 2),
            })

    # Para grafos grandes, all-pairs Dijkstra es demasiado costoso (O(N² log N))
    if num_nodes >= LARGE_GRAPH_THRESHOLD:
        algorithms = ["connected_components", "centrality"]
    else:
        algorithms = ["shortest_paths", "connected_components", "centrality"]

    payload = {
        "type": "ANALYZE_GRAPH",
        "request_id": f"bench-{num_nodes}",
        "payload": {
            "nodes": nodes,
            "edges": edges,
            "algorithms": algorithms,
        },
    }
    return payload


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Genera un grafo JSON para el benchmark del HPC Engine."
    )
    parser.add_argument("num_nodes", type=int, help="Número de nodos del grafo")
    parser.add_argument("output_file", help="Archivo JSON de salida")
    parser.add_argument(
        "--seed",
        type=int,
        default=None,
        help=(
            "Semilla del generador aleatorio. Con la misma semilla, tamaño y densidad se obtiene "
            "exactamente el mismo grafo, lo que permite comparar mediciones entre máquinas. "
            "Sin semilla, cada ejecución genera un grafo distinto."
        ),
    )
    parser.add_argument(
        "--density",
        type=float,
        default=2.0,
        help=(
            "Factor de densidad de aristas extra respecto al número de nodos "
            "(default: 2.0 → ~2×N aristas adicionales). "
            "Para grafos de 100k nodos se recomienda 0.5 o menos."
        ),
    )
    args = parser.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    payload = generate_graph(args.num_nodes, args.density)
    edge_count = len(payload["payload"]["edges"])
    algorithms = payload["payload"]["algorithms"]

    with open(args.output_file, "w") as f:
        json.dump(payload, f)

    print(
        f"Generated '{args.output_file}': {args.num_nodes} nodes, "
        f"{edge_count} edges, algorithms={algorithms}"
    )


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
import json
import random
import sys

def generate_graph(num_nodes):
    nodes = [f"N{i}" for i in range(num_nodes)]
    edges = []
    
    # Ensure it's connected (tree structure)
    for i in range(1, num_nodes):
        parent = random.randint(0, i - 1)
        edges.append({
            "from": nodes[parent],
            "to": nodes[i],
            "weight": round(random.uniform(1.0, 10.0), 2)
        })
        # Add reverse edge for undirected connectivity
        edges.append({
            "from": nodes[i],
            "to": nodes[parent],
            "weight": round(random.uniform(1.0, 10.0), 2)
        })

    # Add random extra edges for density (~3 edges per node)
    extra_edges = num_nodes * 2
    for _ in range(extra_edges):
        u = random.randint(0, num_nodes - 1)
        v = random.randint(0, num_nodes - 1)
        if u != v:
            edges.append({
                "from": nodes[u],
                "to": nodes[v],
                "weight": round(random.uniform(1.0, 10.0), 2)
            })

    payload = {
        "type": "ANALYZE_GRAPH",
        "request_id": f"bench-{num_nodes}",
        "payload": {
            "nodes": nodes,
            "edges": edges,
            "algorithms": ["shortest_paths", "connected_components", "centrality"]
        }
    }
    
    return payload

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: generate_graph.py <num_nodes> <output_file>")
        sys.exit(1)
        
    num_nodes = int(sys.argv[1])
    output_file = sys.argv[2]
    
    payload = generate_graph(num_nodes)
    
    with open(output_file, 'w') as f:
        json.dump(payload, f)
    
    print(f"Generated {output_file} with {num_nodes} nodes and {len(payload['payload']['edges'])} edges.")

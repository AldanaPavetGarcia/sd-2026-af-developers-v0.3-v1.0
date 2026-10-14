#!/usr/bin/env python3
"""Contract test for the api-gateway delivery layouts (ADR-011), on the rendered manifests.

kubeconform proves the objects are valid Kubernetes; this proves they route traffic the way the
canary procedure assumes:

  * two Deployments whose selectors cannot overlap, the canary at 0 replicas (steady state);
  * the shared Service api-gateway selects both tracks (US-304 AC7), each per-track Service only
    its own;
  * the main Ingress targets stable, the canary Ingress the canary with weight 0, on the same
    host and path (US-304 AC1, AC2);
  * every gateway pod keeps what makes a switch error-free: readiness probe, preStop sleep and a
    rolling update that never removes a ready pod first (ADR-011 §5).

Needs kubectl (for `kubectl kustomize`) and PyYAML.
"""

import subprocess
import sys

import yaml

CANARY_OVERLAY = "k8s/api-gateway/overlays/canary"
WEIGHT = "nginx.ingress.kubernetes.io/canary-weight"


def fail(message):
    print(f"ERROR: {message}")
    sys.exit(1)


def render(path):
    out = subprocess.run(["kubectl", "kustomize", path], capture_output=True, text=True, check=False)
    if out.returncode != 0:
        fail(f"kubectl kustomize {path} failed: {out.stderr}")
    objects = {}
    for doc in yaml.safe_load_all(out.stdout):
        if doc:
            objects[(doc["kind"], doc["metadata"]["name"])] = doc
    return objects


def get(objects, kind, name):
    obj = objects.get((kind, name))
    if obj is None:
        fail(f"{kind} {name} is missing")
    return obj


def selects(selector, labels):
    return all(labels.get(key) == value for key, value in selector.items())


def check_safe_rollout(name, deployment):
    spec = deployment["spec"]
    strategy = spec.get("strategy", {}).get("rollingUpdate", {})
    if strategy.get("maxUnavailable") != 0:
        fail(f"{name}: rolling update must keep maxUnavailable 0")
    container = spec["template"]["spec"]["containers"][0]
    if container.get("readinessProbe", {}).get("httpGet", {}).get("path") != "/v1/health":
        fail(f"{name}: readiness probe must be GET /v1/health")
    if "sleep" not in container.get("lifecycle", {}).get("preStop", {}):
        fail(f"{name}: preStop sleep is missing")


def check_canary(objects):
    pods = {}
    for track in ("stable", "canary"):
        name = f"api-gateway-{track}"
        deployment = get(objects, "Deployment", name)
        selector = deployment["spec"]["selector"]["matchLabels"]
        labels = deployment["spec"]["template"]["metadata"]["labels"]
        if selector.get("track") != track or labels.get("track") != track:
            fail(f"{name}: selector and pod labels must carry track={track}")
        if not selects(selector, labels):
            fail(f"{name}: its selector does not match its own pods")
        check_safe_rollout(name, deployment)
        pods[track] = (selector, labels)

    if selects(pods["stable"][0], pods["canary"][1]) or selects(pods["canary"][0], pods["stable"][1]):
        fail("stable and canary Deployments select each other's pods")
    if get(objects, "Deployment", "api-gateway-canary")["spec"].get("replicas") != 0:
        fail("the canary must be versioned at 0 replicas")

    shared = get(objects, "Service", "api-gateway")["spec"]["selector"]
    if not all(selects(shared, labels) for _, labels in pods.values()):
        fail("Service api-gateway must select both tracks (US-304 AC7)")
    for track in ("stable", "canary"):
        selector = get(objects, "Service", f"api-gateway-{track}")["spec"]["selector"]
        other = "canary" if track == "stable" else "stable"
        if not selects(selector, pods[track][1]) or selects(selector, pods[other][1]):
            fail(f"Service api-gateway-{track} must select only the {track} pods")

    main = get(objects, "Ingress", "api-gateway")
    canary = get(objects, "Ingress", "api-gateway-canary")
    annotations = canary["metadata"].get("annotations", {})
    if annotations.get("nginx.ingress.kubernetes.io/canary") != "true":
        fail("Ingress api-gateway-canary must be marked canary")
    if annotations.get(WEIGHT) != "0":
        fail(f"the versioned canary weight must be 0, got {annotations.get(WEIGHT)!r}")
    if "nginx.ingress.kubernetes.io/canary" in main["metadata"].get("annotations", {}):
        fail("the main Ingress must not be a canary")
    routes = {}
    for name, ingress in (("main", main), ("canary", canary)):
        if ingress["spec"].get("ingressClassName") != "nginx":
            fail(f"{name} Ingress must use the nginx class")
        rule = ingress["spec"]["rules"][0]
        path = rule["http"]["paths"][0]
        routes[name] = (rule["host"], path["path"], path["backend"]["service"]["name"])
    if routes["main"][:2] != routes["canary"][:2]:
        fail(f"both Ingresses must share host and path: {routes}")
    if routes["main"][2] != "api-gateway-stable" or routes["canary"][2] != "api-gateway-canary":
        fail(f"Ingress backends are wrong: {routes}")


def main():
    check_canary(render(CANARY_OVERLAY))
    print(f"[gateway-overlays-test] contract passed: {CANARY_OVERLAY}")


if __name__ == "__main__":
    main()

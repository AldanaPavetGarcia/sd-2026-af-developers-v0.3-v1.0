#!/usr/bin/env python3
"""Contract test for the api-gateway delivery layouts (ADR-011), on the rendered manifests.

kubeconform proves the objects are valid Kubernetes; this proves they route traffic the way the
canary and blue-green procedures assume. Canary:

  * two Deployments whose selectors cannot overlap, the canary at 0 replicas (steady state);
  * the shared Service api-gateway selects both tracks (US-304 AC7), each per-track Service only
    its own;
  * the main Ingress targets stable, the canary Ingress the canary with weight 0, on the same
    host and path (US-304 AC1, AC2);
  * every gateway pod keeps what makes a switch error-free: readiness probe, preStop sleep and a
    rolling update that never removes a ready pod first (ADR-011 §5).

Blue-green:

  * two complete Deployments, blue and green, with disjoint selectors (US-305 AC1);
  * the Ingress goes to api-gateway-active, which selects blue (US-305 AC2) and nothing else;
    no Service selects both slots;
  * each preview Service selects only its slot (the smoke tests' way in, AC3).

Needs kubectl (for `kubectl kustomize`) and PyYAML.
"""

import subprocess
import sys

import yaml

CANARY_OVERLAY = "k8s/api-gateway/overlays/canary"
BLUE_GREEN_OVERLAY = "k8s/api-gateway/overlays/blue-green"
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


def check_blue_green(objects):
    pods = {}
    for slot in ("blue", "green"):
        name = f"api-gateway-{slot}"
        deployment = get(objects, "Deployment", name)
        selector = deployment["spec"]["selector"]["matchLabels"]
        labels = deployment["spec"]["template"]["metadata"]["labels"]
        if selector.get("slot") != slot or labels.get("slot") != slot:
            fail(f"{name}: selector and pod labels must carry slot={slot}")
        if labels.get("track") != slot:
            fail(f"{name}: track must name the slot, so telemetry and X-EOP-Track tell slots apart")
        check_safe_rollout(name, deployment)
        pods[slot] = (selector, labels)
    if selects(pods["blue"][0], pods["green"][1]) or selects(pods["green"][0], pods["blue"][1]):
        fail("blue and green Deployments select each other's pods")
    if get(objects, "Deployment", "api-gateway-blue")["spec"].get("replicas", 1) < 1:
        fail("blue must be versioned serving")

    for (kind, name), obj in objects.items():
        if kind == "Service":
            selector = obj["spec"]["selector"]
            if selects(selector, pods["blue"][1]) and selects(selector, pods["green"][1]):
                fail(f"Service {name} selects both slots: it would expose green before its smoke tests")

    active = get(objects, "Service", "api-gateway-active")["spec"]["selector"]
    if active.get("slot") != "blue" or not selects(active, pods["blue"][1]):
        fail(f"api-gateway-active must select blue, got {active}")
    for slot in ("blue", "green"):
        preview = get(objects, "Service", f"api-gateway-{slot}-preview")["spec"]["selector"]
        if not selects(preview, pods[slot][1]):
            fail(f"api-gateway-{slot}-preview must select the {slot} pods")

    ingress = get(objects, "Ingress", "api-gateway")
    backend = ingress["spec"]["rules"][0]["http"]["paths"][0]["backend"]["service"]["name"]
    if backend != "api-gateway-active":
        fail(f"the Ingress must target api-gateway-active, got {backend}")
    if any(kind == "Ingress" and name != "api-gateway" for kind, name in objects):
        fail("the blue-green layout must not carry a second Ingress")


def main():
    check_canary(render(CANARY_OVERLAY))
    check_blue_green(render(BLUE_GREEN_OVERLAY))
    print(f"[gateway-overlays-test] contract passed: {CANARY_OVERLAY}, {BLUE_GREEN_OVERLAY}")


if __name__ == "__main__":
    main()

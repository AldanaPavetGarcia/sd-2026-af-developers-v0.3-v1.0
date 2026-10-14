#!/usr/bin/env python3
"""Smoke tests of an API gateway deployment before it receives traffic (US-305 AC3, AC6).

Runs against one URL (in a blue-green switch, the preview Service of the slot about to go live)
and checks the three things US-305 AC6 names:

  1. health: GET /v1/health answers 200 with status ok or degraded (eop-server reachable);
  2. node listing: GET /v1/nodes answers 200 with a list;
  3. a sample graph analysis: POST /v1/graph/analyze with a three-node graph answers 200 with the
     connected components the graph actually has.

With --expect-version, every response must also come from that version (X-EOP-Version), so the
suite cannot pass against the slot that is already serving. Exits 0 only if every check passes.

Usage: gateway_smoke.py URL [--expect-version V] [--host HOST]
"""

import argparse
import json
import sys
import urllib.error
import urllib.request

TIMEOUT_SECONDS = 10
SAMPLE_GRAPH = {
    "nodes": ["vault-a", "vault-b", "vault-c"],
    "edges": [{"from": "vault-a", "to": "vault-b", "weight": 1}],
    "algorithms": ["connected_components"],
}


def call(base, method, path, host=None, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base.rstrip("/") + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if host:
        req.add_header("Host", host)
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_SECONDS) as resp:
            return resp.status, resp.headers, json.loads(resp.read() or b"null")
    except urllib.error.HTTPError as err:
        return err.code, err.headers, json.loads(err.read() or b"null")
    except (urllib.error.URLError, OSError, ValueError) as err:
        return None, {}, str(err)


class Suite:
    def __init__(self, base, host, expect_version):
        self.base, self.host, self.expect_version = base, host, expect_version
        self.passed = True

    def check(self, label, ok, detail=""):
        print(("OK:   " if ok else "FAIL: ") + label + ("" if ok else f" {detail}"))
        self.passed &= ok

    def request(self, label, method, path, body=None):
        status, headers, payload = call(self.base, method, path, self.host, body)
        if self.expect_version is not None and status is not None:
            served = headers.get("X-EOP-Version")
            self.check(f"{label}: served by version {self.expect_version}", served == self.expect_version,
                       f"(X-EOP-Version: {served})")
        return status, payload

    def run(self):
        status, payload = self.request("health", "GET", "/v1/health")
        state = payload.get("data", {}).get("status") if isinstance(payload, dict) else None
        self.check("health answers 200 ok/degraded", status == 200 and state in ("ok", "degraded"),
                   f"(got {status} {payload})")

        status, payload = self.request("nodes", "GET", "/v1/nodes")
        nodes = payload.get("data") if isinstance(payload, dict) else None
        self.check("node listing answers 200 with a list", status == 200 and isinstance(nodes, list),
                   f"(got {status} {payload})")

        status, payload = self.request("analysis", "POST", "/v1/graph/analyze", SAMPLE_GRAPH)
        result = payload.get("data", {}) if isinstance(payload, dict) else {}
        components = (result.get("result") or result).get("algorithms", {}).get("connected_components")
        self.check("sample analysis answers 200", status == 200, f"(got {status} {payload})")
        # vault-a and vault-b are joined, vault-c is alone: the engine must find exactly two.
        groups = components.get("components") if isinstance(components, dict) else components
        self.check("sample analysis finds the graph's 2 components",
                   isinstance(groups, list) and len(groups) == 2, f"(got {components})")
        return self.passed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("url")
    parser.add_argument("--expect-version", default=None)
    parser.add_argument("--host", default=None, help="Host header (when going through an Ingress)")
    args = parser.parse_args()
    if Suite(args.url, args.host, args.expect_version).run():
        print("OK: smoke tests passed")
        return 0
    print("FAIL: smoke tests failed")
    return 1


if __name__ == "__main__":
    sys.exit(main())

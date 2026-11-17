#!/usr/bin/env python3
"""US-405 AC6: 5 minutes of synthetic edge load, and a summary that matches the dashboard.

Runs edge-sim (docker-compose profile edge-sim) for EDGE_TEST_SECONDS with EDGE_TEST_DEVICES
devices, each with a node_id prefix unique to this run, then reads Prometheus:

* messages: what the simulator published must equal what the ingestor received and forwarded
  (exact: counter values before and after, no extrapolation), with no invalid messages;
* rate: the "Edge Messages per Second" panel's expression, averaged over the run, must match the
  real average (published / duration) within EDGE_TEST_RATE_TOLERANCE;
* latency: P50 and P99 of this run's devices from the histogram the panels use; P99 must stay
  below 500 ms (the NFR, and the alert's threshold);
* loss: the dashboard's loss-rate expression must read 0 %.

Needs the compose stack (make edge-test starts it): eop-server, redis, mosquitto, edge-ingestor,
prometheus, and optionally api-gateway for /v1/edge/status.
"""

import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request

PROMETHEUS = os.environ.get("PROMETHEUS_URL", "http://localhost:9090")
GATEWAY = os.environ.get("GATEWAY_URL", "http://localhost:8080")
SECONDS = int(os.environ.get("EDGE_TEST_SECONDS", "300"))
DEVICES = int(os.environ.get("EDGE_TEST_DEVICES", "5"))
RATE_TOLERANCE = float(os.environ.get("EDGE_TEST_RATE_TOLERANCE", "0.10"))
LATENCY_LIMIT_S = 0.5
SCRAPE_WAIT_S = 90
STEP = "15s"

DASHBOARD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                         "monitoring", "grafana", "dashboards", "eop-edge-dashboard.json")


def panel_exprs(path=DASHBOARD):
    """The dashboard's queries by panel title and legend, so the test reads what Grafana shows."""
    with open(path, encoding="utf-8") as f:
        dashboard = json.load(f)
    out = {}
    for panel in dashboard["panels"]:
        for target in panel.get("targets", []):
            out[(panel["title"], target.get("legendFormat", target["refId"]))] = target["expr"]
    return out


def query(expr, base=PROMETHEUS):
    url = base + "/api/v1/query?" + urllib.parse.urlencode({"query": expr})
    with urllib.request.urlopen(url, timeout=10) as resp:
        body = json.load(resp)
    if body.get("status") != "success":
        raise RuntimeError(f"Prometheus error for {expr}: {body}")
    result = body["data"]["result"]
    return float(result[0]["value"][1]) if result else 0.0


def counters(prefix):
    selector = f'{{node_id=~"{prefix}.*"}}'
    return {
        "received": query("sum(eop_edge_messages_received_total)"),
        "forwarded": query("sum(eop_edge_messages_forwarded_total)"),
        "invalid": query("sum(eop_edge_messages_invalid_total)"),
        "duplicates": query("sum(eop_edge_messages_duplicate_total)"),
        "latency_count": query(f"sum(eop_edge_e2e_latency_seconds_count{selector})"),
    }


def summarize(before, after, published, seconds, panel_rate, p50, p99, loss_percent):
    """Pure comparison: the numbers and the list of mismatches (empty when the run passes)."""
    delta = {key: after[key] - before[key] for key in before}
    summary = {
        "published": published,
        "received": delta["received"],
        "forwarded": delta["forwarded"],
        "invalid": delta["invalid"],
        "duplicates": delta["duplicates"],
        "latency_samples": delta["latency_count"],
        "rate_real": published / seconds,
        "rate_panel": panel_rate,
        "p50_s": p50,
        "p99_s": p99,
        "loss_percent": loss_percent,
    }
    errors = []
    for key in ("received", "forwarded", "latency_samples"):
        if summary[key] != published:
            errors.append(f"{key} = {summary[key]:.0f}, simulator published {published}")
    if summary["invalid"] != 0:
        errors.append(f"{summary['invalid']:.0f} invalid messages")
    if abs(panel_rate - summary["rate_real"]) > RATE_TOLERANCE * summary["rate_real"]:
        errors.append(f"panel rate {panel_rate:.2f} msg/s vs real {summary['rate_real']:.2f} msg/s")
    if not p99 < LATENCY_LIMIT_S:
        errors.append(f"P99 {p99 * 1000:.0f} ms is not below {LATENCY_LIMIT_S * 1000:.0f} ms")
    if loss_percent != 0:
        errors.append(f"loss rate {loss_percent:.2f} %")
    return summary, errors


def gateway_devices(prefix):
    try:
        with urllib.request.urlopen(GATEWAY + "/v1/edge/status", timeout=5) as resp:
            data = json.load(resp)["data"]
    except OSError:
        return None
    return sorted(d["node_id"] for d in data.get("devices", []) if d["node_id"].startswith(prefix))


def main():
    prefix = f"edgetest-{int(time.time())}-"
    exprs = panel_exprs()
    before = counters(prefix)
    started = time.time()
    print(f"[edge-test] {DEVICES} devices x {SECONDS} samples ({prefix}*), about {SECONDS // 60} min...", flush=True)
    subprocess.run(["docker", "compose", "--profile", "edge-sim", "run", "--rm",
                    "-e", f"EOP_SIM_DEVICES={DEVICES}", "-e", f"EOP_SIM_COUNT={SECONDS}",
                    "-e", f"EOP_SIM_NODE_PREFIX={prefix}", "edge-sim"], check=True)
    published = DEVICES * SECONDS
    seconds = time.time() - started

    deadline = time.time() + SCRAPE_WAIT_S
    after = counters(prefix)
    while after["forwarded"] - before["forwarded"] < published and time.time() < deadline:
        time.sleep(5)
        after = counters(prefix)
    devices = gateway_devices(prefix)

    window = f"{int(seconds)}s"
    selector = f'{{node_id=~"{prefix}.*"}}'
    rate_expr = exprs[("Edge Messages per Second", "received")]
    panel_rate = query(f"avg_over_time(({rate_expr})[{window}:{STEP}])")
    p50 = query(f"histogram_quantile(0.50, sum by (le) (increase(eop_edge_e2e_latency_seconds_bucket{selector}[{window}])))")
    p99 = query(f"histogram_quantile(0.99, sum by (le) (increase(eop_edge_e2e_latency_seconds_bucket{selector}[{window}])))")
    loss = query(exprs[("Edge Message Loss Rate (buffer overflow)", "A")])

    summary, errors = summarize(before, after, published, seconds, panel_rate, p50, p99, loss)
    print("[edge-test] summary (Prometheus, as the EOP Edge Telemetry dashboard reads it):")
    for key, value in summary.items():
        print(f"  {key:16} {value:.3f}" if isinstance(value, float) else f"  {key:16} {value}")
    if devices is not None:
        print(f"  gateway devices  {len(devices)} of {DEVICES} in /v1/edge/status")
    if errors:
        sys.exit("[edge-test] FAIL: " + "; ".join(errors))
    print("[edge-test] PASS")


if __name__ == "__main__":
    main()

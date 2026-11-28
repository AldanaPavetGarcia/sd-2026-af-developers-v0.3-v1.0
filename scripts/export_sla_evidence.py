#!/usr/bin/env python3
"""Exports the evidence of the SLA period as a deliverable (US-407, #75).

For the "Vault-Tec Delivery" dashboard it writes, into one directory:

* dashboard.json      the dashboard as Grafana serves it (GET /api/dashboards/uid/<uid>);
* <panel>.csv         every panel's queries over the period, from Prometheus' query_range
                      (timestamp, series, value), so the data outlives the cluster;
* summary.json        the headline numbers at the end of the period (instant queries): success
                      ratio, error budget left, availability, last MTTR, edge P99;
* dashboard.png       a screenshot through Grafana's render API, when the image renderer plugin
                      is installed; otherwise SCREENSHOT.txt says how to take one by hand.

Usage (make sla-export port-forwards Grafana and Prometheus and calls it):

  export_sla_evidence.py --grafana http://127.0.0.1:3000 --prometheus http://127.0.0.1:9090 \\
      --out evidence/ [--days 30] [--step 1h] [--end <unix seconds>]

Grafana credentials: EOP_GRAFANA_USER / EOP_GRAFANA_PASSWORD (default admin/admin).
"""

import argparse
import base64
import csv
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DASHBOARD_UID = "eop-vault-tec-delivery"
HTTP_TIMEOUT_S = 60
RENDER_TIMEOUT_S = 120
RENDER_WIDTH = 1800
RENDER_HEIGHT = 2400
SECONDS = {"s": 1, "m": 60, "h": 3600, "d": 86400}
# Headline numbers of summary.json: name -> instant PromQL.
# The recording rules run once an hour, so they are read through last_over_time: a plain instant
# query only looks 5 minutes back.
SUMMARY_QUERIES = {
    "eop_server_success_ratio_30d": "min(last_over_time(eop:requests_success:ratio30d[2h]))",
    "error_budget_remaining_30d": "min(last_over_time(eop:error_budget_remaining:ratio30d[2h]))",
    "gateway_success_ratio_30d": "last_over_time(eop:gateway_success:ratio30d[2h])",
    "eop_server_availability_30d": 'avg(last_over_time(eop:up:avg30d{job="eop-server"}[2h]))',
    "mttr_last_seconds": 'eop_mttr_last_seconds{kind="service_restoration"}',
    "replica_replacement_last_seconds": 'eop_mttr_last_seconds{kind="replica_replacement"}',
    "edge_e2e_p99_seconds_1h": "histogram_quantile(0.99, sum by (le) (rate(eop_edge_e2e_latency_seconds_bucket[1h])))",
}


def duration_seconds(text):
    match = re.fullmatch(r"(\d+)([smhd])", text)
    if not match:
        raise ValueError(f"bad duration {text!r}: use e.g. 30s, 5m, 1h, 30d")
    return int(match.group(1)) * SECONDS[match.group(2)]


def substitute(expr, days, step):
    """Grafana's template variables, as the dashboard would resolve them over the period."""
    return (expr.replace("$__range", f"{days}d")
                .replace("$__rate_interval", "5m")
                .replace("$__interval", step))


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "panel"


class Client:
    def __init__(self, base, user=None, password=None):
        self.base = base.rstrip("/")
        self.auth = None
        if user is not None:
            token = base64.b64encode(f"{user}:{password}".encode()).decode()
            self.auth = f"Basic {token}"

    def get(self, path, params=None, timeout=HTTP_TIMEOUT_S):
        url = self.base + path + ("?" + urllib.parse.urlencode(params) if params else "")
        request = urllib.request.Request(url)
        if self.auth:
            request.add_header("Authorization", self.auth)
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.headers.get("Content-Type", ""), response.read()

    def get_json(self, path, params=None):
        _, body = self.get(path, params)
        return json.loads(body)


def prometheus_data(prom, path, params):
    answer = prom.get_json(path, params)
    if answer.get("status") != "success":
        raise RuntimeError(f"Prometheus {path} {params.get('query')}: {answer}")
    return answer["data"]["result"]


def series_name(metric):
    labels = {k: v for k, v in metric.items() if k != "__name__"}
    return ",".join(f"{k}={v}" for k, v in sorted(labels.items())) or metric.get("__name__", "value")


def export_panels(prom, dashboard, out, start, end, days, step):
    """One CSV per panel with every target's range query. Returns the files written."""
    written = []
    step_s = duration_seconds(step)
    for panel in dashboard.get("panels", []):
        targets = [t for t in panel.get("targets", []) if t.get("expr")]
        if not targets:
            continue
        name = f"{panel['id']:02d}-{slug(panel['title'])}.csv"
        with open(os.path.join(out, name), "w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(["query", "series", "timestamp", "value"])
            for target in targets:
                query = substitute(target["expr"], days, step)
                result = prometheus_data(prom, "/api/v1/query_range",
                                         {"query": query, "start": start, "end": end, "step": step_s})
                for series in result:
                    for timestamp, value in series.get("values", []):
                        writer.writerow([target.get("refId", ""), series_name(series["metric"]),
                                         int(float(timestamp)), value])
        written.append(name)
    return written


def export_summary(prom, out, end):
    summary = {"at": end}
    for key, query in SUMMARY_QUERIES.items():
        result = prometheus_data(prom, "/api/v1/query", {"query": query, "time": end})
        summary[key] = float(result[0]["value"][1]) if result else None
    with open(os.path.join(out, "summary.json"), "w") as handle:
        json.dump(summary, handle, indent=2)
        handle.write("\n")
    return summary


def export_screenshot(grafana, out, start, end):
    """dashboard.png through the render API, or SCREENSHOT.txt when it is not available."""
    params = {"orgId": 1, "from": int(start) * 1000, "to": int(end) * 1000,
              "width": RENDER_WIDTH, "height": RENDER_HEIGHT, "kiosk": "true"}
    try:
        content_type, body = grafana.get(f"/render/d/{DASHBOARD_UID}", params, timeout=RENDER_TIMEOUT_S)
        if content_type.startswith("image/png"):
            with open(os.path.join(out, "dashboard.png"), "wb") as handle:
                handle.write(body)
            return "dashboard.png"
        reason = f"render API answered {content_type}"
    except (urllib.error.URLError, OSError) as error:
        reason = f"render API unavailable ({error})"
    with open(os.path.join(out, "SCREENSHOT.txt"), "w") as handle:
        handle.write(
            f"No screenshot was taken: {reason}.\n"
            "Grafana renders images only with the grafana-image-renderer plugin. Take it by hand:\n"
            f"  1. open {grafana.base}/d/{DASHBOARD_UID}?from={int(start) * 1000}&to={int(end) * 1000}&kiosk\n"
            "  2. browser full-page screenshot (Firefox: Ctrl+Shift+S, 'Save full page')\n"
            "  3. save it here as dashboard.png\n")
    return "SCREENSHOT.txt"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--grafana", required=True)
    parser.add_argument("--prometheus", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--days", type=int, default=30)
    parser.add_argument("--step", default="1h")
    parser.add_argument("--end", type=float, default=None, help="end of the period, unix seconds (default: now)")
    args = parser.parse_args(argv)

    end = args.end if args.end is not None else time.time()
    start = end - args.days * SECONDS["d"]
    os.makedirs(args.out, exist_ok=True)
    grafana = Client(args.grafana, os.environ.get("EOP_GRAFANA_USER", "admin"),
                     os.environ.get("EOP_GRAFANA_PASSWORD", "admin"))
    prom = Client(args.prometheus)

    dashboard = grafana.get_json(f"/api/dashboards/uid/{DASHBOARD_UID}")["dashboard"]
    with open(os.path.join(args.out, "dashboard.json"), "w") as handle:
        json.dump(dashboard, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    files = export_panels(prom, dashboard, args.out, start, end, args.days, args.step)
    summary = export_summary(prom, args.out, end)
    shot = export_screenshot(grafana, args.out, start, end)

    print(f"sla-export: {len(files)} panel CSVs, summary.json, dashboard.json and {shot} in {args.out}")
    for key, value in summary.items():
        if key != "at":
            print(f"  {key}: {'n/a' if value is None else round(value, 6)}")
    budget = summary.get("error_budget_remaining_30d")
    if budget is not None and budget <= 0:
        print("sla-export: WARNING: the error budget is exhausted (US-407 AC1 needs it above 0)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""SLA report of a partial SLO period from the hourly series saved on the lab machine (#67, US-407).

Input: a directory with one Prometheus answer per recording rule, saved as raw-<rule>.json, where
<rule> is the rule name with ':' written as '_' (for example raw-eop_requests_increase1h.json) and
the answer is /api/v1/query?query=<rule>[10d] (a matrix of the hourly samples).

Output, into --out:

* slo-series.json  the same shape `make slo-run-export` writes: {"exported_at", "step_seconds",
                   "series": {rule: [matrix rows]}}, so the period can be reported without the
                   cluster;
* slo-report.json  the figures of the period: hours covered and missing, requests, success ratio
                   and error budget left of eop-server, API gateway success ratio, availability by
                   job, and the last 30-day values the rules recorded, for comparison;
* slo-report.md    the same figures as a Markdown table for the documentation.

Usage: slo_partial_report.py --raw <dir> --out <dir> [--start "2026-11-19 00:47 UTC"]
"""

import argparse
import datetime
import json
import os
import sys

HOUR_S = 3600
SLO_TARGET = 0.999
RULES = (
    "eop:requests:increase1h", "eop:requests_ok:increase1h", "eop:gateway_requests:increase1h",
    "eop:gateway_requests_5xx:increase1h", "eop:up:avg1h", "eop:requests_success:ratio30d",
    "eop:error_budget_remaining:ratio30d", "eop:gateway_success:ratio30d", "eop:up:avg30d",
)


def file_name(rule):
    return "raw-" + rule.replace(":", "_") + ".json"


def load_rows(path):
    """The matrix rows of a saved answer, or [] when the file is missing."""
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as handle:
        answer = json.load(handle)
    if isinstance(answer, list):
        return answer
    if answer.get("status") != "success":
        raise ValueError(f"{path}: Prometheus answered {answer.get('status')}: {answer.get('error')}")
    data = answer["data"]
    if data.get("resultType") != "matrix":
        raise ValueError(f"{path}: expected a matrix (query <rule>[10d]), got {data.get('resultType')}")
    return data["result"]


def samples(rows, label=None):
    """{label value: {hour timestamp: value}} of the rows, keyed by `label` (None: one key)."""
    out = {}
    for row in rows:
        key = row["metric"].get(label, "all") if label else "all"
        series = out.setdefault(key, {})
        for timestamp, value in row.get("values", []):
            series[int(float(timestamp)) // HOUR_S * HOUR_S] = float(value)
    return out


def utc(timestamp):
    return datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def budget_left(ratio):
    return max(0.0, 1 - (1 - ratio) / (1 - SLO_TARGET))


def last_values(rows, label):
    return {key: series[max(series)] for key, series in samples(rows, label).items() if series}


def build(series, start=None):
    requests = samples(series["eop:requests:increase1h"], "service")
    ok = samples(series["eop:requests_ok:increase1h"], "service")
    if not requests:
        raise ValueError("eop:requests:increase1h has no samples: nothing to report")
    hours = sorted({h for per in requests.values() for h in per})
    first, last = hours[0], hours[-1]
    expected = (last - first) // HOUR_S + 1
    missing = [utc(h) for h in range(first, last + HOUR_S, HOUR_S) if h not in set(hours)]

    report = {
        "start": start or utc(first - HOUR_S),
        "first_hour": utc(first), "last_hour": utc(last),
        "hours_recorded": len(hours), "hours_expected": expected, "hours_missing": missing,
        "days": round(len(hours) / 24, 2),
        "eop_server": {}, "notes": [],
    }
    for service, per in sorted(requests.items()):
        total = sum(per.values())
        good = sum(ok.get(service, {}).values())
        ratio = good / total if total else None
        report["eop_server"][service] = {
            "requests": round(total), "failed": round(total - good),
            "requests_per_hour": round(total / len(per)),
            "success_ratio": ratio,
            "error_budget_remaining": budget_left(ratio) if ratio is not None else None,
        }

    gateway = samples(series["eop:gateway_requests:increase1h"]).get("all", {})
    errors = samples(series["eop:gateway_requests_5xx:increase1h"]).get("all", {})
    if gateway:
        total = sum(gateway.values())
        bad = sum(errors.values())
        if not errors:
            report["notes"].append(
                "eop:gateway_requests_5xx:increase1h has no samples: before the fix the rule recorded "
                "nothing when there was no 5xx, so the period counts 0 gateway 5xx.")
        report["gateway"] = {"requests": round(total), "5xx": round(bad),
                             "success_ratio": 1 - bad / total if total else None}

    up = samples(series["eop:up:avg1h"], "job")
    report["availability"] = {job: sum(per.values()) / len(per) for job, per in sorted(up.items()) if per}
    report["recorded_30d"] = {
        "eop:requests_success:ratio30d": last_values(series["eop:requests_success:ratio30d"], "service"),
        "eop:error_budget_remaining:ratio30d": last_values(series["eop:error_budget_remaining:ratio30d"], "service"),
        "eop:gateway_success:ratio30d": last_values(series["eop:gateway_success:ratio30d"], None),
        "eop:up:avg30d": last_values(series["eop:up:avg30d"], "job"),
    }
    if missing:
        report["notes"].append(f"{len(missing)} hour(s) without a recorded sample: {', '.join(missing)}")
    return report


def percent(value):
    return "n/a" if value is None else f"{value * 100:.3f} %"


def markdown(report):
    lines = [
        f"| SLO period | {report['start']} → {report['last_hour']} ({report['days']} days, "
        f"{report['hours_recorded']} of {report['hours_expected']} hours recorded) |",
        "|---|---|",
    ]
    for service, figures in report["eop_server"].items():
        lines += [
            f"| {service} requests | {figures['requests']:,} ({figures['requests_per_hour']:,}/h), "
            f"{figures['failed']:,} failed |",
            f"| {service} success ratio (SLO 99.9 %) | {percent(figures['success_ratio'])} |",
            f"| {service} error budget left | {percent(figures['error_budget_remaining'])} |",
        ]
    if "gateway" in report:
        gateway = report["gateway"]
        lines.append(f"| API gateway success ratio (no 5xx) | {percent(gateway['success_ratio'])} "
                     f"({gateway['requests']:,} requests, {gateway['5xx']:,} 5xx) |")
    for job, value in report["availability"].items():
        lines.append(f"| Availability {job} | {percent(value)} |")
    text = "\n".join(lines) + "\n"
    if report["notes"]:
        text += "\n" + "\n".join(f"- {note}" for note in report["notes"]) + "\n"
    return text


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--raw", required=True, help="directory with the raw-<rule>.json files")
    parser.add_argument("--out", required=True)
    parser.add_argument("--start", default=None, help="start of the run (slo-start.txt)")
    args = parser.parse_args(argv)

    series, absent = {}, []
    for rule in RULES:
        rows = load_rows(os.path.join(args.raw, file_name(rule)))
        if rows is None:
            absent.append(file_name(rule))
            rows = []
        series[rule] = rows
    if absent:
        print(f"slo-partial-report: missing files (treated as empty): {', '.join(absent)}", file=sys.stderr)

    try:
        report = build(series, args.start)
    except ValueError as error:
        print(f"slo-partial-report: {error}", file=sys.stderr)
        return 1
    os.makedirs(args.out, exist_ok=True)
    stamps = [float(v[0]) for rows in series.values() for row in rows for v in row.get("values", [])]
    with open(os.path.join(args.out, "slo-series.json"), "w", encoding="utf-8") as handle:
        json.dump({"exported_at": int(max(stamps)), "step_seconds": HOUR_S, "series": series}, handle)
    with open(os.path.join(args.out, "slo-report.json"), "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2)
        handle.write("\n")
    text = markdown(report)
    with open(os.path.join(args.out, "slo-report.md"), "w", encoding="utf-8") as handle:
        handle.write(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())

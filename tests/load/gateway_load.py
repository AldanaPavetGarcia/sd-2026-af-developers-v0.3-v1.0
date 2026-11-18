#!/usr/bin/env python3
"""Continuous load on the API gateway through the Ingress, attributing every response to the
deployment that served it (X-EOP-Version / X-EOP-Track, ADR-011).

Two modes:

  load (default)   Keeps --concurrency clients sending requests until --duration elapses or the
                   process gets SIGINT/SIGTERM. Prints the split per version and track every
                   --report-every seconds and a summary at the end. Exits 1 if any request got a
                   5xx or a connection error: this is the "no client errors during the rollout"
                   check of US-304 AC6 and US-305 AC4. The summary also gives the client-side
                   latency (P50/P95/P99) per path; with --max-p99 MS the run fails if the P99 of
                   any path exceeds MS (the v0.3 NFR: REST API P99 < 300 ms, ADR-010 §6).

  --until-absent T Sends requests one after another until --settle consecutive responses come
                   from a track other than T, then prints how long that took. make
                   canary-rollback uses it to measure the rollback (US-304 AC4, < 30 s).

The target defaults to the minikube Ingress: http://$(minikube ip) with Host: api.eop.local.
"""

import argparse
import collections
import http.client
import math
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.parse

DEFAULT_HOST = "api.eop.local"
DEFAULT_PATHS = "/v1/health,/v1/nodes"
REQUEST_TIMEOUT_SECONDS = 5


def default_url():
    url = os.environ.get("EOP_INGRESS_URL")
    if url:
        return url
    try:
        ip = subprocess.run(["minikube", "ip"], capture_output=True, text=True, timeout=20, check=True).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        ip = "127.0.0.1"
    return f"http://{ip}"


class Client:
    """One keep-alive connection, reopened after any failure."""

    def __init__(self, url, host):
        parsed = urllib.parse.urlsplit(url)
        self.netloc = parsed.netloc
        self.https = parsed.scheme == "https"
        self.host = host
        self.conn = None
        self.elapsed = None

    def get(self, path):
        """Returns (status, track, version), or (None, None, error) on a connection error."""
        self.elapsed = None
        try:
            if self.conn is None:
                factory = http.client.HTTPSConnection if self.https else http.client.HTTPConnection
                self.conn = factory(self.netloc, timeout=REQUEST_TIMEOUT_SECONDS)
            started = time.perf_counter()
            self.conn.request("GET", path, headers={"Host": self.host})
            resp = self.conn.getresponse()
            resp.read()
            self.elapsed = time.perf_counter() - started
            if resp.getheader("Connection", "").lower() == "close":
                self.close()
            return resp.status, resp.getheader("X-EOP-Track", "-"), resp.getheader("X-EOP-Version", "-")
        except (OSError, http.client.HTTPException) as err:
            self.close()
            return None, None, f"{type(err).__name__}: {err}"

    def close(self):
        if self.conn is not None:
            self.conn.close()
            self.conn = None


class Tally:
    def __init__(self):
        self.lock = threading.Lock()
        self.total = collections.Counter()
        self.window = collections.Counter()
        self.errors = []
        self.latencies = collections.defaultdict(list)

    def record(self, status, track, version, path, elapsed=None):
        with self.lock:
            if elapsed is not None:
                self.latencies[path].append(elapsed)
            if status is None:
                key = "connection-error"
                self.errors.append(f"{path}: {version}")
            elif status >= 500:
                key = f"{version} ({track}) 5xx"
                self.errors.append(f"{path}: HTTP {status} from {version} ({track})")
            else:
                key = f"{version} ({track})"
            self.total[key] += 1
            self.window[key] += 1

    def take_window(self):
        with self.lock:
            window, self.window = self.window, collections.Counter()
        return window


def describe(counter):
    count = sum(counter.values())
    if not count:
        return "no requests"
    parts = [f"{key}: {n} ({100 * n / count:.0f} %)" for key, n in sorted(counter.items())]
    return f"{count} requests | " + " | ".join(parts)


def percentile(samples, q):
    """Nearest-rank percentile of a sorted list."""
    return samples[max(0, math.ceil(q / 100 * len(samples)) - 1)]


def latency_report(latencies, max_p99_ms):
    """Prints P50/P95/P99 per path; returns the paths whose P99 exceeds max_p99_ms."""
    over = []
    for path, samples in sorted(latencies.items()):
        samples = sorted(samples)
        p50, p95, p99 = (1000 * percentile(samples, q) for q in (50, 95, 99))
        print(f"latency {path}: n={len(samples)} p50={p50:.1f} ms p95={p95:.1f} ms p99={p99:.1f} ms")
        if max_p99_ms and p99 > max_p99_ms:
            over.append(f"{path} p99 {p99:.1f} ms > {max_p99_ms:g} ms")
    return over


def run_load(args):
    paths = [p for p in args.paths.split(",") if p]
    tally = Tally()
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())

    def worker(index):
        client = Client(args.url, args.host)
        n = index
        while not stop.is_set():
            path = paths[n % len(paths)]
            n += 1
            status, track, version = client.get(path)
            tally.record(status, track, version, path, client.elapsed)
            if args.interval:
                stop.wait(args.interval)
        client.close()

    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(args.concurrency)]
    for thread in threads:
        thread.start()
    print(f"load: {args.concurrency} clients on {args.url} (Host: {args.host}), paths {paths}", flush=True)
    started = time.monotonic()
    while not stop.is_set():
        stop.wait(args.report_every)
        print(f"[{time.monotonic() - started:6.1f} s] {describe(tally.take_window())}", flush=True)
        if args.duration and time.monotonic() - started >= args.duration:
            stop.set()
    for thread in threads:
        thread.join(REQUEST_TIMEOUT_SECONDS + 1)

    print(f"summary: {describe(tally.total)}")
    over = latency_report(tally.latencies, args.max_p99)
    if tally.errors:
        print(f"FAIL: {len(tally.errors)} client errors, first ones:")
        for line in tally.errors[:10]:
            print(f"  {line}")
    else:
        print("OK: no 5xx and no connection errors")
    for line in over:
        print(f"FAIL: {line}")
    if args.max_p99 and not over:
        print(f"OK: p99 of every path under {args.max_p99:g} ms")
    return 1 if tally.errors or over else 0


def run_until_absent(args):
    client = Client(args.url, args.host)
    # Wall clock, so the caller can pass the moment its change started (--since).
    started = args.since or time.time()
    streak = 0
    first_clean = None
    while time.time() - started < args.timeout:
        status, track, _ = client.get("/v1/health")
        if status is not None and track != args.until_absent:
            if streak == 0:
                first_clean = time.time()
            streak += 1
            if streak >= args.settle:
                print(f"no '{args.until_absent}' responses {first_clean - started:.2f} s after the change "
                      f"(confirmed by {args.settle} consecutive requests; limit {args.timeout:g} s)")
                return 0
        else:
            streak = 0
        time.sleep(args.probe_interval)
    print(f"FAIL: still getting '{args.until_absent}' responses {args.timeout:g} s after the change")
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default=None, help="Ingress URL (default: EOP_INGRESS_URL or http://$(minikube ip))")
    parser.add_argument("--host", default=DEFAULT_HOST, help="Host header the Ingress routes on")
    parser.add_argument("--paths", default=DEFAULT_PATHS, help="comma-separated paths, requested in turn")
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--interval", type=float, default=0.05, help="pause between requests of one client, s")
    parser.add_argument("--duration", type=float, default=0, help="seconds to run; 0 runs until Ctrl-C")
    parser.add_argument("--report-every", type=float, default=5)
    parser.add_argument("--max-p99", type=float, default=0, help="fail if any path's P99 exceeds this, ms")
    parser.add_argument("--until-absent", metavar="TRACK", help="wait until TRACK stops answering")
    parser.add_argument("--settle", type=int, default=40, help="consecutive responses that confirm it")
    parser.add_argument("--probe-interval", type=float, default=0.05)
    parser.add_argument("--timeout", type=float, default=60, help="give up this many seconds after --since")
    parser.add_argument("--since", type=float, default=0, help="epoch seconds the change started (default: now)")
    args = parser.parse_args()
    args.url = args.url or default_url()
    return run_until_absent(args) if args.until_absent else run_load(args)


if __name__ == "__main__":
    sys.exit(main())

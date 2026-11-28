# 30-day SLO run (#67, US-407 AC1/AC2)

US-407 shows the SLA with 30 days of SLO data at delivery. The data is collected on the 14-core
machine by the current stack (v0.3) with three additions, and the stack is upgraded in place to each
v1.0 build without losing what was collected:

| Piece | Where | What it does |
|---|---|---|
| Persistent Prometheus | `k8s/prometheus-deployment.yaml`, `k8s/prometheus-pvc.yaml` | TSDB on a 10 Gi PersistentVolumeClaim, 35-day retention (capped at 8 GB), `Recreate` so the volume is never shared by two Pods |
| Recording rules | `k8s/prometheus-rules-configmap.yaml` | Hourly increases of the request counters and the 30-day figures: `eop:requests_success:ratio30d`, `eop:error_budget_remaining:ratio30d` (SLO 99.9 %), `eop:gateway_success:ratio30d`, `eop:up:avg30d` |
| Steady traffic | `k8s/slo-run/slo-traffic.yaml`, `services/go/cmd/slo-traffic` | 3 bunker nodes registered on eop-server with a heartbeat every 5 s, a gateway read every second (`/v1/health`, `/v1/nodes`, `/v1/nodes/{id}` in turn) and a 3-node graph analysis every 5 minutes |

The load is about 1.3 requests per second, ~110,000 a day: enough for every SLO panel to have data
all the time, and about 3 % of the 3.4 million log lines a day that ADR-025 rejected for a
permanent 20 ms prober. `slo-traffic` logs only failures and one summary every 10 minutes; its own
view of each request is in `eop_traffic_requests_total{target,op,result}` and
`eop_traffic_request_duration_seconds`.

## 1. Prepare the machine (once)

Docker, minikube and kubectl, and a clone of this repository:

```bash
git clone https://github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0.git eop && cd eop
git checkout feature/67-slo-30-day-run     # or development, once this PR and v0.3 are merged
```

Start minikube with room for the whole stack and a month of data. With 14 cores, give it 10 and
leave the rest to the host; memory as available (12 GB is comfortable, 8 GB is the minimum):

```bash
minikube start --cpus 10 --memory 12g --disk-size 60g
```

Build the images inside minikube's Docker, so `imagePullPolicy: IfNotPresent` finds them:

```bash
eval "$(minikube docker-env)"
docker build -t eop-server:local .
docker build -t hpc-engine:local -f hpc-engine/Dockerfile .
make gateway-image          # api-gateway, external-stub and slo-traffic
```

The machine must not suspend: disable sleep and automatic updates that reboot it. A reboot is not
fatal (section 4), but every hour down is an hour with no data, which the report has to declare.

## 2. Start the run

```bash
make slo-run-start
```

It applies the whole stack (`make k8s-apply`: eop-server, hpc-engine, Redis, gateway, Prometheus with
its volume, Grafana, Loki, SigNoz) and then `k8s/slo-run/`. Write down the date and time it
started: it is the start of the SLO period.

## 3. Check that it is collecting

```bash
make slo-run-status
```

```text
[slo-run] data since 2026-11-17 14:02 UTC (0.1 days of the 30)
[slo-run] TSDB blocks on the volume: 0 MiB
[slo-run] up eop-server eop-server-6d9…: 1
[slo-run] up slo-traffic slo-traffic-7c4…: 1
[slo-run] synthetic traffic, last 10 min: 780 requests, 0 failed; bunker nodes registered: 3
[slo-run] eop:requests_success:ratio30d: not recorded yet (first value after 1 h)
[slo-run] collecting
```

(Example output from the first minutes of a run.) It exits 1 if the PVC is not bound or no synthetic request arrived in the last 10 minutes. Run it
once a day; Grafana shows the same data:

```bash
kubectl port-forward service/grafana 3000:3000     # http://localhost:3000, EOP SLO Dashboard
```

The days counted come from `prometheus_tsdb_lowest_timestamp`: the oldest sample on the volume,
from Prometheus' own `prometheus` scrape job.

**Reading the recorded series.** The rules run once an hour, so each `eop:*` series has one sample
an hour. A plain instant query looks back only 5 minutes and misses it most of the time, and
`query_range` does the same at each step. Read them through a range selector:
`last_over_time(eop:requests_success:ratio30d[2h])` for the latest value, `eop:requests:increase1h[35d]`
for every hourly sample. The dashboard, `slo-run-status`, `slo-run-export` and `sla-export` all do,
and `make prometheus-rules-test` checks the rules with promtool.

## 4. Restarts and upgrades to v1.0, without losing the data

What keeps the data is the `prometheus-data` PersistentVolumeClaim. It survives Prometheus
restarts, `make k8s-apply`, `minikube stop` / `minikube start` and a host reboot (minikube keeps its
hostpath volumes).

**Never** run `minikube delete`, `kubectl delete -f k8s/` or `kubectl delete pvc prometheus-data`:
each one deletes the volume and the run starts over.

After a reboot:

```bash
minikube start
make slo-run-status
```

To move to a new v1.0 build:

```bash
git pull                     # the branch or tag of the new build
eval "$(minikube docker-env)"
docker build -t eop-server:local . && docker build -t hpc-engine:local -f hpc-engine/Dockerfile .
make gateway-image
make slo-run-start           # applies the new manifests; the PVC and its data stay
kubectl rollout restart deployment/eop-server deployment/api-gateway-stable deployment/slo-traffic
kubectl rollout restart deployment/prometheus deployment/grafana   # only if their config or rules changed
make slo-run-status          # same "data since" as before the upgrade
```

Prometheus and Grafana read their configuration, rules and dashboards only at start, so a change
to them needs the restart above (`scripts/lab.sh update` always does it). Prometheus then loses only
the seconds it is down (`Recreate`). Scrape targets that v1.0 adds (the hpc-engine sidecar, mttr-watcher, the edge
ingestor) simply start having data from the upgrade on.

## 5. End of the run: export the data

```bash
make slo-run-export
```

It writes to `slo-run-export/` (ignored by git):

* `tsdb-<snapshot>.tar.gz`: a consistent snapshot of the whole TSDB (admin API). To look at it on
  any machine: unpack it and run
  `prometheus --storage.tsdb.path=tsdb-<snapshot> --config.file=monitoring/prometheus/prometheus.yml`.
* `slo-series.json`: every hourly sample of the recording rules in the last 35 days (`<rule>[35d]`),
  the input of the US-407 SLA report.

For a period saved as one Prometheus answer per rule (`raw-<rule>.json`, `:` written as `_`, from
`/api/v1/query?query=<rule>[10d]`), `make slo-partial-report RAW=<dir> OUT=<dir> START="<start>"`
writes the same `slo-series.json` plus `slo-report.json` and `slo-report.md`: hours recorded and
missing, requests, success ratio and error budget left, gateway success ratio and availability.

## 6. Declaring the period

Issue #67 planned the start for 2026-11-08 at the latest. The run started on
**2026-11-19 00:47 UTC** on the 14-core machine (`docs/LAB-14-CORE.md`, "The current run"), so the
delivery shows the days actually collected (`make slo-run-status`) and says so: the report states the
real start date and the gaps, never extrapolated figures.

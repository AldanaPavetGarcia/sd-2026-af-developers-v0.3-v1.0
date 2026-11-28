# SLA demo to Vault-Tec (US-407)

The final demo of EOP v1.0: four live steps on the production stack, each one shown in the
**Vault-Tec Delivery — EOP v1.0** Grafana dashboard, plus the evidence of the 30-day SLA period.
`make demo-script` runs the steps one by one with a pause before each; `DEMO_AUTO=1` runs them
without pauses for rehearsals.

| Step | What the audience sees | Pass criterion | Where |
|---|---|---|---|
| 1. A replica dies | `kubectl delete pod` of an eop-server replica while a probe calls it every 20 ms | probe outage and mttr-watcher's service restoration < 200 ms | Failover row |
| 2. Canary release | API gateway at 10 → 50 → 100 % with clients calling | no 5xx or connection error; per-version error rate and P99 flat | Canary row |
| 3. One edge message | a simulated bunker → Mosquitto → ingestor → eop-server → `/v1/nodes`, and its trace in SigNoz | the device shows up with its telemetry within 60 s | Edge row, SigNoz |
| 4. Smoke test | `make smoke-test-prod` | 5 PASS | terminal |

The 30-day row of the dashboard (success ratio, error budget, availability, hourly series) comes
from the recording rules of `k8s/prometheus-rules-configmap.yaml`; the error budget must be above 0
at delivery (US-407 AC1).

## Before the day: rehearsal on the 14-core machine

The stack must have been running since the start of the SLO period (`docs/slo-30-day-run.md`).

1. **Stack and images.** `kubectl get pods` shows everything Running; `make k8s-prod-apply` again
   if anything changed (it keeps the Prometheus PVC). The canary image exists in the cluster
   (`DEMO_CANARY_IMAGE`, default `api-gateway:local` relabelled as `DEMO_CANARY_VERSION`, default
   `1.0.1`), and so do `edge-sim:local` and `mttr-watcher:local`.
2. **Ingress.** `minikube addons enable ingress` and `curl -H 'Host: api.eop.local' http://$(minikube ip)/v1/health`
   answers 200 (the canary step sends its load through the Ingress).
3. **Full rehearsal.** `DEMO_AUTO=1 make demo-script` ends with "Demo complete: the cluster is
   healthy." Run it twice: the second run proves it can be repeated.
4. **Grafana and SigNoz open in the browser,** logged in:
   `kubectl port-forward svc/grafana 3000` → *Vault-Tec Delivery — EOP v1.0*;
   `kubectl port-forward svc/signoz 8080` → Traces.
5. **Evidence exported** (below) and the numbers written down, in case the network fails on the day.
6. **Backup:** the evidence directory and a screen recording of a rehearsal on a USB stick.

## On the day: what to say at each step

**Opening (dashboard, 30-day row).** "EOP has been running for N days. The eop-server success ratio
is X %, the API gateway's Y %, against a 99.9 % SLO; Z % of the error budget is still unspent."

**1. A replica dies.** "eop-server runs two replicas that share the node registry in Redis. I delete
one Pod now, while a client calls every 20 milliseconds." After it: "No call failed, and the service
was restored in N ms, under the 200 ms target. Next to it, the replacement Pod took S seconds to be
Ready: that is the PRD's 'pod death to pod ready', which Kubernetes cannot do in 200 ms, and why a
second replica takes over." (ADR-025, ADR-027.)

**2. Canary.** "A new gateway version takes 10 % of the traffic, then half, then all of it, while
clients keep calling. Here are the error rate and the P99 by version: the new one behaves like the
old one, so we promote it." If it misbehaved: `make canary-rollback` (weight 0 in under 30 s).

**3. Edge.** "A bunker sensor publishes its samples to the broker; the ingestor forwards each one
once — duplicates are dropped by message id — and the gateway now shows this bunker with its
temperature. In SigNoz, one trace covers the message from the device's sampling to the server."

**4. Smoke test.** "Every component answers: server, registration, graph analysis, edge and the
public API."

## If something fails live

| Symptom | Do |
|---|---|
| Preflight: a deployment is not Ready | `kubectl get pods`, `kubectl describe pod …`; `make k8s-prod-apply` and wait; skip to the next step with `DEMO_STEPS="2 3 4"` |
| Step 1 over 200 ms | show the probe's line (client outage) and the watcher's; rerun `make k8s-mttr-test` once; otherwise show the rehearsal's numbers from the evidence |
| Step 2: clients saw errors | the script already set the weight back to 0 (rollback); explain the rollback as the safety net, then `make canary-status` |
| Step 3: no telemetry in 60 s | `kubectl logs deploy/edge-ingestor --tail=20`, `kubectl get pods -l app=mosquitto`; `GET /v1/edge/status` tells whether the ingestor is connected |
| Step 4 fails | read which check failed; the cluster stays up; the earlier steps already showed each part |
| Grafana or SigNoz will not open | port-forward again; fall back to the exported evidence |

Any failure stops the script with `DEMO FAILED: <what>` and leaves the cluster healthy (canary
weight 0, demo Pods removed), so `make demo-script` can be run again.

## The evidence of the SLA period

```bash
make sla-export                 # evidence/sla-<UTC date>/
make sla-export OUT=evidence/final SLA_DAYS=30 SLA_STEP=1h
```

It port-forwards Grafana and Prometheus and writes `dashboard.json`, one CSV per panel with the
period's data, `summary.json` (success ratios, error budget, availability, last MTTR, edge P99) and
a screenshot. Grafana takes screenshots only with the `grafana-image-renderer` plugin; without it the
directory has `SCREENSHOT.txt` with the dashboard URL for the period, to take a full-page browser
screenshot by hand. The command fails if the error budget is spent. `evidence/` is not committed:
attach the directory to the delivery.

If the period is shorter than 30 days (the run started late), `SLA_DAYS` is the real number of days
and the delivery says so.

### The period so far

The run started on 2026-11-19 00:47 UTC (`docs/LAB-14-CORE.md`). On 2026-11-27, the status on the
lab machine and the hourly series saved that day gave:

| SLO | Target | 2026-11-19 00:47 → 2026-11-27 UTC (8.96 days, 215 hourly samples) |
|---|---|---|
| eop-server success ratio | ≥ 99.9 % | 100 % (about 6,500 requests an hour, none failed) |
| Error budget left | > 0 | 100 % |
| API gateway success ratio (no 5xx) | ≥ 99.9 % | 100 %: no 5xx was recorded in the period |
| Availability (`up`) | ≥ 99.9 % | 100 % for each of the 6 jobs: eop-server, hpc-engine, api-gateway, slo-traffic, mttr-watcher, edge-ingestor |

#### Events of the period

| When (UTC) | Event | Effect on the data |
|---|---|---|
| 2026-11-19 00:47 | Start of the run (`scripts/lab.sh start`) | Start of the SLO period |
| 2026-11-27 23:57 | First extraction, with the old readers | None: it found the read bugs (`docs/LAB-14-CORE.md`) |
| 2026-11-28 ~00:38 | `scripts/lab.sh update` to the fixes: every EOP Deployment restarted (eop-server with its 2 replicas one at a time) | 6 failed synthetic requests out of 958 in the following 10 minutes, the first failures of the run. To be checked in the next extraction: whether they reached eop-server or the gateway as errors (`eop:requests_success`, `eop:gateway_success`) or only the client (`eop_traffic_requests_total`) |
| 2026-11-28 00:45:08 | Prometheus and Grafana restarted by hand to load the new configuration and rules | About 1–2 minutes without scrapes |
| 2026-12-19 00:47 | Planned end of the 30 days (2026-12-18 21:47 ART) | |

The table above covers up to the first extraction, before these restarts. The restarts are
maintenance of the measurement itself, not a failure of the product, but they count against the SLO
like any other failure: the report states them and does not remove them.

`make slo-partial-report RAW=<dir> OUT=<dir> START="2026-11-19 00:47 UTC"` recomputes the table
from the saved `raw-eop_*.json` files, hour by hour, and lists any hour without a sample. The table
above is replaced by its `slo-report.md`, and at the end of the run by `make sla-export` with the
final number of days.

# EOP Runbook

Operational procedures for EOP v1.0 on Kubernetes (US-406 AC5). The product is deployed with
`make k8s-prod-apply` (`kubectl apply -k deploy/production`, README "Deploy EOP v1.0"); the
development path `make k8s-apply` deploys the same workloads, so every procedure applies to both.
`kubectl` must point at the cluster.

## Deploying and changing the configuration

- **Deploy or upgrade:** `make k8s-prod-apply`, then `make smoke-test-prod`. Applying again is
  safe: unchanged objects are left alone, changed ones roll out with the same probes and budgets.
- **Change a setting:** edit `config/production.env` (ports, retention, SLO and alert thresholds,
  registry and edge tuning), run `make production-config-test`, then `make k8s-prod-apply`.
  Pods read the `eop-global-config` ConfigMap at start: restart the affected Deployment afterwards
  (`kubectl rollout restart deployment/<name>`). An alert threshold is also in its rule file under
  `monitoring/grafana/provisioning/alerting/`; the contract test fails until both agree.
- **What is persistent:** Prometheus (`prometheus-data`, 35 days), Loki (`loki-data`), ClickHouse
  (`clickhouse-data`, traces and logs of SigNoz), ZooKeeper (`zookeeper-data`), Mosquitto (queued
  messages and its TLS material). Never delete the PVCs or run `kubectl delete -k
  deploy/production` on a cluster whose history must be kept (the 30-day SLO evidence).

## Scaling services

| Service | How | Notes |
|---|---|---|
| eop-server | `kubectl scale deployment/eop-server --replicas=N` | ≥ 2 keeps failover (ADR-027); every replica brings its own hpc-engine sidecar; the PodDisruptionBudget keeps one Ready during drains |
| API gateway | `kubectl scale deployment/api-gateway-stable --replicas=N` | stateless; jobs are in Redis. During a canary scale the track being changed (ADR-011) |
| Edge ingestor | keep **1** | one consumer of the broker's persistent session processes messages in order (ADR-028 §2); more needs shared subscriptions |
| Mosquitto, Redis, Prometheus, Loki, ClickHouse, ZooKeeper | keep 1 | single-instance stateful services with their volume |
| Simulated devices | `kubectl scale deployment/edge-sim --replicas=1` | off in production; 5 synthetic bunkers for demos and dashboards |

Resources are in each manifest (`resources:`); the HPC engine's CPU limit must stay ≥
`OMP_NUM_THREADS` in `config/production.env`.

## Rolling back

- **One service:** `kubectl rollout undo deployment/<name>` returns to the previous ReplicaSet;
  `kubectl rollout history deployment/<name>` lists them. eop-server rolls with
  `maxUnavailable: 0`, so service continues during the undo.
- **API gateway:** use the canary or blue-green procedures below (seconds, with checks).
- **The whole product:** check out the previous release tag and `make k8s-prod-apply`; images
  are pinned in `deploy/production/kustomization.yaml` (`images:`), so the previous release's
  overlay brings back its images and configuration together. Data on the PVCs is kept.
- **A bad configuration value:** revert the line in `config/production.env` and apply again.

## Reading the SLO dashboard

Grafana → Dashboards → EOP → *EOP SLO Dashboard* (`docs/slo-dashboard-grafana.md` has every panel).

- **Service Availability** (target 99.9 %, `EOP_SLO_AVAILABILITY_TARGET`): share of eop-server
  requests that succeeded. Green above target.
- **Error Budget Remaining:** how much of the period's 0.1 % of allowed failures is left. Below
  50 % the `eop-error-budget-below-50` alert fires: slow down releases and look at *Failed
  Requests* and *Error Rate* to find the cause. At 0 % the SLO is broken for the period.
- **P99 Request Latency** (eop-server, target < 200 ms) and **Gateway P99 Latency by Version**
  (target < 300 ms): a rise on one version only, during a rollout, means roll that version back.
- **MTTR on Pod Failure:** time until the Service stopped routing to a failed eop-server Pod
  (target < 200 ms, red above); *Replica Replacement Time* next to it is the PRD's "pod death to
  pod ready" (seconds, informative).
- **30-day figures:** Prometheus records them hourly (`k8s/prometheus-rules-configmap.yaml`):
  `eop:requests_success:ratio30d`, `eop:error_budget_remaining:ratio30d`,
  `eop:gateway_success:ratio30d`, `eop:up:avg30d`. Query them in Grafana → Explore, or run
  `make slo-run-status` (docs/slo-30-day-run.md).

Edge telemetry has its own dashboard, *EOP Edge Telemetry* (`docs/edge-latency-dashboard.md`):
end-to-end P99 against 500 ms, messages per second, buffer overflow losses and the device table.

## Failover and MTTR (ADR-027)

- Two eop-server replicas share the node registry in Redis. When one fails, the Service stops
  routing to it and the other keeps serving; nodes whose session was on the failed replica read
  OFFLINE within 3 s and are taken over when they register again.
- `make k8s-mttr-test` deletes one Pod under a 20 ms probe and fails at an MTTR of 200 ms or more
  (`docs/kubernetes-deployment-and-pod-recovery-guide.md` §10b).
- **Redis down:** both replicas leave the Service (their readiness depends on the registry) and
  commands answer `ERROR_SERVICE_UNAVAILABLE`; they come back on their own when Redis does, and
  rewrite their live nodes into it. Check `kubectl get pods -l app=redis`.

## Edge devices (US-404, US-405)

- **Is ingestion working?** `kubectl port-forward svc/api-gateway 8080:80` and
  `curl -s localhost:8080/v1/edge/status`: `state` `connected`, `received` and `forwarded`
  growing. `disconnected` means the ingestor is not writing its status (check
  `kubectl logs deployment/edge-ingestor`).
- **eop-server down:** the ingestor retries each message with backoff and logs every attempt; the
  broker keeps the backlog. Nothing to do once the server is back.
- **New device:** it needs the broker's CA (`kubectl exec mosquitto-0 -- cat
  /mosquitto/certs/ca.crt`) and connects to the node's port 30883 with TLS. To use your own
  certificates, create the `mosquitto-tls` Secret (`ca.crt`, `tls.crt`, `tls.key`) and restart
  the broker (`kubectl rollout restart statefulset/mosquitto`).

## From a Grafana alert to the request logs (US-306)

Logs of every pod are collected by the `otel-log-agent` DaemonSet into Loki (ADR-026). Each EOP
log line carries `service`, `level`, `trace_id` and `span_id`; a request that goes through the
API gateway keeps one `trace_id` across the gateway and `eop-server`.

1. **Open the alert.** Grafana → Alerting shows the firing rule (for example
   `eop-error-budget-below-50`) and its time window.
2. **Find the failing service and the moment.** Grafana → Dashboards → *EOP Request Logs*:
   - set *Level* to `ERROR` and the time range to the alert's window;
   - the *Error Lines per Service* panel shows which service started failing and when.
3. **Pick a failing request.** In *Request Logs*, expand an ERROR line. Its fields include
   `trace_id`, `node_id` and `event` (the operation).
4. **See the whole request.** The line's *Open trace in SigNoz* link opens the trace
   (`kubectl port-forward svc/signoz 8081:8080` first). The trace shows every service the
   request crossed, with `result=ERROR` on the failed span.
5. **From a trace back to its logs.** Copy the trace ID shown in SigNoz into the *trace_id* box
   of *EOP Request Logs*: only the lines of that request remain, across services. An API
   response also carries it, in `meta.trace_id`.

Equivalent query in Grafana → Explore → Loki:

```logql
{service_name=~".+"} | trace_id="<trace id>"
```

Useful filters: `{service_name="eop-server"} | level="ERROR"`,
`{service_name="api-gateway"} | route="/v1/nodes/{node_id}" | status="404"`.

**Checks**

- `kubectl get pods -l app=otel-log-agent` — one Running pod per node.
- `kubectl port-forward svc/loki 3100 && curl -s localhost:3100/loki/api/v1/label/service_name/values`
  — lists every service that sent logs.
- `python3 tests/k8s/log-trace-correlation.py` (or `make k8s-log-trace-test`) — sends a failing
  request and checks that SigNoz and Loki hold it under the same `trace_id` (US-306 AC6).

**Retention.** 7 days, enforced by Loki's compactor; data lives on the `loki-data`
PersistentVolumeClaim. In the product it is `EOP_LOKI_RETENTION` in `config/production.env`;
the development path takes it from `monitoring/loki/config.yaml`.

## Canary release of the API gateway (US-304)

The gateway runs in the canary layout of ADR-011: `api-gateway-stable` serves all traffic,
`api-gateway-canary` waits at 0 replicas, and the Ingress `api-gateway-canary` (host
`api.eop.local`) holds the weight, 0 outside a rollout. A rollout only changes that weight.

**Before you start**

- ingress-nginx is installed: `minikube addons enable ingress` (once per cluster).
- The new image is available to the cluster (`minikube image load api-gateway:<tag>`).
- Grafana → *EOP SLO Dashboard*, row of *Gateway … by Version* panels: error rate, P99 and
  requests per second per version and track.
- SigNoz (`kubectl port-forward svc/signoz 8081:8080`) → Traces, service `api-gateway`, grouped
  by `service.version` (or `deployment.track`): the same comparison from the traces.
- In a second terminal, the load test: `make gateway-load-test`. It prints the split per
  version every 5 s and fails on any 5xx or connection error. Leave it running until the end.

**Procedure**

1. `make canary-deploy IMAGE=api-gateway:<tag> VERSION=<version>` — starts one canary pod
   with the new version; it gets no traffic yet.
2. `make canary-rollout WEIGHT=10` — 10 % of requests go to the canary. The command refuses a
   weight above 0 while the canary has no ready pod.
3. Watch the per-version panels for at least 5 minutes. Then `WEIGHT=50`, watch again, and
   `WEIGHT=100`.
4. `make canary-promote` — stable is rolled to the canary's image and version (rolling update
   behind its readiness probe), the weight goes back to 0 and the canary to 0 replicas.
5. Stop the load test with Ctrl-C: it must print `OK: no 5xx and no connection errors`.

`make canary-status` prints the weight and the image, version and ready pods of each track.

### Canary rollback

**Trigger:** the Grafana alert *API gateway canary error rate above 1%* (5xx share of the canary
track over 5 minutes, `monitoring/grafana/provisioning/alerting/eop-gateway-alerts.yaml`), or
anything on the per-version panels that looks worse than stable.

**Action:** `make canary-rollback`. It sets the weight to 0 and keeps probing through the
Ingress until 40 consecutive responses come from stable, then prints how long clients kept
reaching the canary after the command started. The limit is 30 s. The canary pod keeps running
for diagnosis (`kubectl logs deploy/api-gateway-canary`, or *EOP Request Logs* filtered by a
`trace_id` from a failed response); scale it down with
`kubectl scale deploy/api-gateway-canary --replicas=0` when done.

**Do not** run `make k8s-apply` during a rollout: re-applying the overlay resets the weight to
the versioned value, 0.

## Blue-green release of the API gateway (US-305)

In the blue-green layout of ADR-011 the Ingress sends every request to the Service
`api-gateway-active`, whose selector names one slot (`slot=blue` or `slot=green`). A release goes
to the idle slot, is smoke-tested there through its own preview Service
(`api-gateway-<slot>-preview`), and goes live with a single patch of that selector.

**Before you start**

- Move the gateway to this layout, outside the demo window: `make blue-green-layout`. It starts
  `api-gateway-blue` with the version that was serving, makes it active and removes the canary
  objects. The way back is `make canary-layout`. Neither move is zero-downtime.
- In a second terminal, the load test: `make gateway-load-test`. It shows which slot answers
  (`0.3.1 (blue)`, `0.3.2 (green)`) and fails on any 5xx or connection error.

**Procedure**

1. `make blue-green-deploy IMAGE=api-gateway:<tag> VERSION=<version>` — the idle slot (green
   while blue is active, and the other way round) starts the new version. It gets no traffic.
2. `make blue-green-switch` — runs `tests/smoke/gateway_smoke.py` against the idle slot's preview
   Service: health, node listing and a three-node graph analysis, every response required to
   come from the new version. If any check fails, nothing changes and the active slot keeps
   serving. If all pass, the selector of `api-gateway-active` moves to the new slot, and the
   command waits until no client is served by the old one and prints how long it took.
3. Verify on the *Gateway … by Version* panels and in the load test output.
4. The old slot keeps running so a rollback needs no pod to start. Once the new version is
   trusted, free its resources with `kubectl scale deploy/api-gateway-<old slot> --replicas=0`.

`make blue-green-status` prints the active slot and the image, version and ready pods of each.

### Blue-green rollback

`make blue-green-rollback` moves the selector back to the other slot — the same mechanism as
the switch, without smoke tests, since that slot was serving moments before — and reports how
long clients kept reaching the slot being left. The limit is 10 s. It refuses if the other slot
has no ready pod (scaled down already): scale it up first.

**Do not** run `make k8s-apply` or re-apply the blue-green overlay during a release: it
returns the gateway to the canary layout, or the active selector to blue.

# EOP Runbook

Operational procedures for the EOP stack on Kubernetes. Every section assumes the stack was
deployed with `make k8s-apply` and that `kubectl` points at the cluster. More sections are added
as the delivery procedures land (the full v1.0 runbook comes with US-406).

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

**Retention.** 7 days, set in `monitoring/loki/config.yaml` and enforced by Loki's compactor;
data lives on the `loki-data` PersistentVolumeClaim. Change it in that file and run
`make k8s-apply`.

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

# EOP Runbook

Operational procedures for the EOP stack on Kubernetes. Every section assumes the stack was
deployed with `make k8s-apply` and that `kubectl` points at the cluster. More sections are added
as the delivery procedures land (canary and blue-green, v0.3; the full v1.0 runbook, US-406).

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

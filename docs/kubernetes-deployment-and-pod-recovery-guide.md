# EOP Deployment and Pod Recovery Guide

This document describes how to build, deploy, and validate the Exodus Ops Platform (EOP) v0.1 server using Docker, Docker Compose, and Kubernetes.

It also documents the local validation workflow for **US-109 · Automatic Pod Recovery**.

---

## 1. Scope

This guide covers:

- building the local EOP server and hpc-engine Docker images;
- running the local Compose stack;
- deploying the EOP server and hpc-engine to Kubernetes (Deployment and Job/batch modes);
- validating liveness and readiness probes for both workloads;
- validating automatic container restart;
- checking pod logs through `kubectl logs`;
- validating client reconnect and node re-registration after restart;
- validating hpc-engine's connect/register retry-backoff and self-healing.

This guide does not cover:

- full OpenTelemetry span validation from US-110;
- production Helm charts or kustomize overlays;
- persistent registry storage;
- API Gateway, MQTT, or later milestone services;
- SLO dashboards / Grafana panels for hpc-engine (issues #17/#18).

---

## 2. Docker Image

The EOP server image is built from the repository root:

```bash
docker build -t eop-server:local .
```

The image is expected to be a multi-stage image:

- the build stage contains compiler and build tools;
- the runtime stage contains only the server binary and required runtime dependencies;
- the runtime image must remain below the 50 MB limit required by US-108.

To inspect the local image size:

```bash
docker images eop-server:local
```

The hpc-engine image is built the same way, from its own dedicated Dockerfile
(build context is still the repo root, since `CMakeLists.txt`/`vcpkg.json` are shared):

```bash
docker build -f hpc-engine/Dockerfile -t hpc-engine:local .
```

Its runtime stage installs `libgomp` in addition to `libstdc++` (OpenMP runtime for the
parallel graph algorithms) and has no `EXPOSE` — hpc-engine never binds or listens, it is
a pure outbound TCP client to eop-server.

```bash
docker images hpc-engine:local
```

---

## 3. Local Compose Deployment

The local development stack can be started with:

```bash
make deploy-local
```

This target runs:

```bash
docker compose up --build -d
./tests/smoke/deploy-local-smoke.sh
```

The smoke test validates that the EOP server accepts TCP connections.

The Compose stack includes:

- EOP server;
- OpenTelemetry Collector;
- SigNoz.

---

## 4. Kubernetes Deployment

The Kubernetes manifests are located under:

```text
k8s/
```

They include:

```text
k8s/configmap.yaml                      # shared eop-global-config ConfigMap (eop-server + hpc-engine)
k8s/deployment.yaml                      # eop-server Deployment
k8s/service.yaml                         # eop-server Service (hpc-engine has no Service — see §6)
k8s/hpc-engine-deployment.yaml           # hpc-engine Deployment (persistent mode)
k8s/prometheus-configmap.yaml            # Prometheus scrape config — see §6c
k8s/prometheus-deployment.yaml           # Prometheus Deployment
k8s/prometheus-service.yaml              # Prometheus Service (fixed name/port, see §6c)
k8s/prometheus-rbac.yaml                 # ServiceAccount/Role/RoleBinding for pod discovery
k8s/grafana-deployment.yaml              # Grafana Deployment (dashboards auto-provisioned)
k8s/grafana-provisioning-configmap.yaml  # Grafana datasource + dashboard-provider ConfigMaps
k8s/grafana-service.yaml                 # Grafana Service
k8s/jobs/                                # hpc-engine Job/batch-mode demo — see §7b, kept out of the
                                          # default sweep below because Job specs are immutable
```

`eop-grafana-dashboards` (the ConfigMap holding the actual dashboard JSON) is **not** a
committed manifest — `make k8s-apply` generates it on the fly from
`monitoring/grafana/dashboards/` (`kubectl create configmap --from-file`) so that directory
stays the single source of truth for panel content, instead of duplicating ~550 lines of JSON
in two places.

Apply the manifests with:

```bash
kubectl apply -f k8s/
```

Or use the Makefile target:

```bash
make k8s-apply
```

The target waits for all four Deployment rollouts:

```bash
kubectl rollout status deployment/eop-server --timeout=60s
kubectl rollout status deployment/hpc-engine --timeout=60s
kubectl rollout status deployment/prometheus --timeout=60s
kubectl rollout status deployment/grafana --timeout=60s
```

`eop-global-config` sets `EOP_SERVER_HOST=eop-server` and pins `--server-port` to
`$(EOP_PORT)` (`9026`) in `hpc-engine-deployment.yaml`'s `args:` — hpc-engine's CLI default
of `9090` does not match the real Service port and must never be relied on.

---

## 5. Minikube Local Setup

The Kubernetes recovery validation was tested with minikube.

Start minikube:

```bash
minikube start --driver=docker
```

Verify that Kubernetes is reachable:

```bash
kubectl get nodes
```

Build the local image:

```bash
docker build -t eop-server:local .
```

Load the image into minikube:

```bash
minikube image load eop-server:local
```

Confirm that the image is available inside minikube:

```bash
minikube image ls | grep eop-server
```

The Kubernetes Deployment uses:

```yaml
image: eop-server:local
imagePullPolicy: IfNotPresent
```

Therefore, the image must exist inside the local cluster before running the manifests.

---

## 6. Kubernetes Health Probes

The EOP server Deployment must define:

- `restartPolicy: Always`;
- a TCP liveness probe on the EOP server port;
- a TCP readiness probe on the EOP server port.

The expected liveness probe configuration is:

```yaml
livenessProbe:
  tcpSocket:
    port: tcp-eop
  initialDelaySeconds: 5
  periodSeconds: 10
  failureThreshold: 3
```

The readiness probe also uses a TCP check against the server port. The Pod must not be marked Ready until the server is ready to accept TCP connections.

### 6a. hpc-engine Probes

hpc-engine never binds or listens (it is a pure outbound client to eop-server), so its
probes cannot use `tcpSocket` checks and instead use `exec`:

```yaml
livenessProbe:
  exec:
    command: ["sh", "-c", "grep -qi hpc_engine /proc/1/comm"]
  initialDelaySeconds: 5
  periodSeconds: 10
  failureThreshold: 3
readinessProbe:
  exec:
    command:
      - sh
      - -c
      - |
        [ -f /tmp/hpc-engine-heartbeat ] && \
        [ $(( $(date +%s) - $(cat /tmp/hpc-engine-heartbeat) )) -lt 20 ]
  initialDelaySeconds: 5
  periodSeconds: 5
  failureThreshold: 12
```

- **Liveness** checks that `hpc_engine` is PID 1 inside the container. This is valid because
  the Dockerfile's `ENTRYPOINT` is exec-form (`["./hpc_engine"]`) with no shell wrapper.
- **Readiness** checks that `hpc_engine` has written to `/tmp/hpc-engine-heartbeat` in the
  last 20 seconds. The binary only starts writing this file *after* it successfully
  registers with eop-server and holds an open session (see §6b) — it is a genuine "actively
  connected" signal, not a fixed always-up check. `failureThreshold: 12` is intentionally
  generous: it covers the worst case of hpc-engine's bounded connect/register retry loop
  (up to 5 attempts with exponential backoff) running before the first heartbeat write.
- Readiness does not gate any Service traffic today — hpc-engine has no Service in front of
  it — but it still drives `kubectl rollout status`, `make k8s-apply`'s wait, and the Ready
  column in `kubectl get pods`.

### 6b. hpc-engine Connect/Register Resilience (Two-Tier Model)

hpc-engine has two distinct resilience tiers, and it is important not to confuse them:

- **Tier 1 — bounded startup retry.** Covers the case where the hpc-engine Pod starts before
  eop-server is Ready (a real race under `kubectl apply -f k8s/`, since both Deployments roll
  out concurrently). `establishSession()` in `hpc-engine/src/main.cpp` retries
  connect+REGISTER up to `--retry-max-attempts` times (default 5) with exponential backoff
  (`--retry-base-delay-ms`, default 250 ms, multiplier 2.0, capped at 5000 ms, ±20% jitter),
  logging one structured JSON line per attempt to stderr. Socket-level `SO_SNDTIMEO`/
  `SO_RCVTIMEO` (3s) bound every `connect()`/`recv()` call during this phase so a single
  attempt can never hang indefinitely.
- **Tier 2 — steady state, no in-process reconnect.** Once registered, the socket timeouts
  are cleared (the request loop must block indefinitely waiting for the next
  `ANALYZE_GRAPH` request) and a clean mid-loop disconnect exits the process with code 0.
  There is no reconnect-inside-the-loop logic — that would duplicate Tier 1's semantics.
  Instead, Kubernetes' `restartPolicy: Always` re-enters `main()` on any process exit,
  which re-runs Tier 1 from scratch. In other words: Kubernetes *is* the Tier 2 recovery
  mechanism, by design.

This is why `hpc-engine-deployment.yaml` does not need an `initContainer` wait-for-eop-server
workaround — the retry loop inside `hpc_engine` itself absorbs the startup race.

### 6c. Metrics Endpoints (Prometheus)

Both workloads expose a Prometheus `/metrics` HTTP endpoint, on a separate port from their
main protocol traffic:

| Workload | Metrics port | env var | Container port name |
|---|---|---|---|
| eop-server | `9100` | `EOP_METRICS_PORT` | `metrics` (`k8s/deployment.yaml`, `k8s/service.yaml`) |
| hpc-engine | `9101` | `EOP_HPC_METRICS_PORT` | `metrics` (`k8s/hpc-engine-deployment.yaml`) |

Both env vars are set in `k8s/configmap.yaml` (`eop-global-config`). Prometheus scrapes each
workload with a **different mechanism**, on purpose — `k8s/prometheus-configmap.yaml` has two
`scrape_configs`:

- **eop-server** uses a `static_configs` target (`eop-server:9100`) against its fixed Service —
  the same approach docker-compose uses, since a stable Service name/port already exists.
- **hpc-engine** has no Service (Deployment pods only — see §4/§6). It advertises itself via
  `prometheus.io/scrape: "true"` / `prometheus.io/path: "/metrics"` / `prometheus.io/port:
  "9101"` pod annotations instead, and Prometheus discovers it dynamically through a
  `kubernetes_sd_configs` (`role: pod`) job with matching `relabel_configs`. This needs the
  RBAC in `k8s/prometheus-rbac.yaml` (a namespaced `Role` granting `get`/`list`/`watch` on
  `pods` to a dedicated `prometheus` ServiceAccount — not cluster-wide).

To verify scraping locally:

```bash
kubectl port-forward svc/prometheus 9090:9090 &
# open http://localhost:9090/targets — eop-server shows up as a fixed target, hpc-engine
# shows up per-pod (its label set includes the actual pod name, not a static job entry)
```

docker-compose only scrapes eop-server — hpc-engine has no docker-compose service (it's a
Kubernetes-only workload by design, see #16), so `monitoring/prometheus/prometheus.yml` (the
compose config) deliberately has a single `scrape_config`.

---

## 7. US-109 Pod Recovery Validation

Run the full pod recovery validation with:

```bash
make k8s-recovery-test
```

This target executes:

```bash
./tests/k8s/pod-recovery-cycle.sh
```

The script validates the following cycle:

1. apply Kubernetes manifests;
2. wait for the EOP server Deployment rollout;
3. validate `restartPolicy`, liveness probe, and readiness probe;
4. reset the Pod to start from a clean state;
5. establish a local `kubectl port-forward`;
6. register a node successfully before the crash;
7. terminate the server process inside the container;
8. wait for Kubernetes to restart the container;
9. measure the time until the Pod is Ready again;
10. verify `kubectl logs --previous` includes the simulated crash reason;
11. reconnect and register a node again after recovery;
12. verify an active client does not hang silently during server termination;
13. delete the Pod with `kubectl delete pod`;
14. wait for the Deployment to create a replacement Pod;
15. register a node successfully against the replacement Pod.

A successful run ends with:

```text
[US-109] US-109 pod recovery cycle passed.
```

The output also includes the measured recovery time, for example:

```text
[US-109] Container restarted and Pod is Ready in 8924 ms.
```

For US-109 AC3, this value must be lower than 10 seconds.

---

## 7a. hpc-engine Pod Recovery Validation

Run the hpc-engine equivalent of the US-109 self-healing check with:

```bash
make k8s-hpc-recovery-test
```

This target executes `./tests/k8s/hpc-engine-recovery-cycle.sh`, which:

1. applies Kubernetes manifests and waits for the hpc-engine Deployment rollout;
2. waits for an hpc-engine Pod to become Ready and confirms its logs show
   `HPC Engine registered as node_id=...`;
3. deletes that Pod;
4. waits for the Deployment to create a replacement Pod and reach Ready;
5. confirms the replacement Pod's logs also show a successful registration.

Unlike the eop-server cycle, there is no active client traffic to observe disconnecting —
hpc-engine carries no client traffic of its own, so the success criterion is simply that the
replacement Pod re-registers with eop-server on its own, driven entirely by Tier 1 of the
retry model in §6b.

## 7b. hpc-engine Job/Batch Mode Demo (AC5)

hpc-engine must run in both a persistent Deployment (§4, validated above) and as a one-shot
Kubernetes `Job` (batch mode). The Job manifests live in `k8s/jobs/`, kept out of the default
`k8s/` sweep because `kubectl apply -f k8s/` is non-recursive and `Job` specs are immutable —
including them there would break re-applying.

Run the demo with:

```bash
make k8s-hpc-batch-demo
```

This target:

```bash
kubectl delete job/hpc-engine-batch-demo --ignore-not-found
kubectl apply -f k8s/jobs/
kubectl wait --for=condition=complete job/hpc-engine-batch-demo --timeout=60s
kubectl logs job/hpc-engine-batch-demo
```

`k8s/jobs/hpc-engine-sample-graph-configmap.yaml` provides a small 5-node/6-edge graph
(`shortest_paths` + `connected_components`) mounted at `/data/graph.json`. The Job overrides
the container's `command` to `./hpc_engine < /data/graph.json` — no `--server-*` args are
passed, so `hasSocketModeArgs()` is false and the binary runs in the same batch mode used for
local testing (read one request from stdin, write one response to stdout, exit). A successful
run prints a single `"status":"ACK"` JSON response and the Job reports `Complete`.

---

## 8. Logs Validation

The recovery script verifies that pod logs are accessible through:

```bash
kubectl logs <pod-name>
```

It also verifies logs from the previous container instance with:

```bash
kubectl logs <pod-name> --previous
```

The simulated termination writes a crash reason before stopping the server process:

```text
US-109 simulated crash reason: forced server process termination
```

The script fails if this reason cannot be found in the previous container logs.

---

## 9. Active Client Behavior

The local validation uses `kubectl port-forward` to connect a local client to the Kubernetes Service.

When the server process terminates while a client is connected, the client may observe one of the following bounded failures depending on how the port-forward proxy closes the connection:

- EOF;
- connection reset;
- timeout within the configured client timeout.

This is accepted for the local validation because the client does not hang silently or indefinitely.

The default active client timeout is controlled by:

```bash
EOP_ACTIVE_CLIENT_TIMEOUT_SECONDS
```

The default value is 8 seconds.

---

## 10. Registry State After Restart

EOP v0.1 stores the node registry in memory.

Therefore, after a container restart or Pod replacement, the registry starts empty. The expected behavior is not state persistence.

The expected recovery behavior is:

1. the client reconnects;
2. the client receives a valid server connection;
3. the client sends `REGISTER` again;
4. the node is registered successfully without manual server intervention.

This behavior is validated by the recovery script through repeated `REGISTER` smoke checks before and after restart events.

---

## 10a. AC7 — Rollout Restart Under Load

Run with:

```bash
make k8s-rollout-restart-test
```

This target executes `./tests/k8s/rollout-restart-under-load.sh`, which applies manifests,
waits for the eop-server rollout, starts a continuous background client issuing `REGISTER`
round-trips, triggers `kubectl rollout restart deployment/eop-server` concurrently, waits for
the new rollout to complete, then stops the client and asserts it observed **zero** failed
operations. EOP has no HTTP/5xx concept, so AC7's "no 5xx" requirement translates directly to
"zero failed/dropped client operations during the rollout".

---

## 11. Useful Commands

Inspect Pods:

```bash
kubectl get pods -l app=eop-server
```

Inspect the Deployment:

```bash
kubectl describe deployment eop-server
```

Inspect a Pod:

```bash
POD="$(kubectl get pods -l app=eop-server -o jsonpath='{.items[0].metadata.name}')"
kubectl describe pod "$POD"
```

Read current logs:

```bash
kubectl logs "$POD"
```

Read previous container logs after a restart:

```bash
kubectl logs "$POD" --previous
```

Delete the Pod and let the Deployment recreate it:

```bash
kubectl delete pod -l app=eop-server
kubectl wait --for=condition=Ready pod -l app=eop-server --timeout=60s
```

Run the register smoke helper directly through an existing port-forward:

```bash
EOP_HOST=127.0.0.1 EOP_PORT=9026 ./tests/smoke/eop-register-smoke.sh
```

hpc-engine equivalents — inspect Pods:

```bash
kubectl get pods -l app=hpc-engine
```

Inspect the Deployment:

```bash
kubectl describe deployment hpc-engine
```

Check the heartbeat/registration signal directly (mirrors the readiness probe in §6a):

```bash
POD="$(kubectl get pods -l app=hpc-engine -o jsonpath='{.items[0].metadata.name}')"
kubectl exec "$POD" -- sh -c 'cat /tmp/hpc-engine-heartbeat; echo'
kubectl logs "$POD" | grep 'HPC Engine registered'
```

---

## 12. CI Policy

The Kubernetes recovery and load-restart validations are intentionally not part of the normal
pull request CI pipeline. They require:

- a running Kubernetes cluster;
- `kubectl`;
- locally available `eop-server:local` / `hpc-engine:local` images;
- port-forwarding support.

For this reason, they are provided as reproducible local/manual validations through:

```bash
make k8s-recovery-test          # US-109, eop-server
make k8s-hpc-recovery-test      # hpc-engine self-healing (§7a)
make k8s-rollout-restart-test   # AC7, rollout restart under load (§10a)
make k8s-hpc-batch-demo         # AC5, Job/batch mode demo (§7b)
```

The normal CI should remain lightweight and should not depend on minikube or local cluster
state — with one exception: `k8s-manifest-lint` (in `.github/workflows/ci.yml`) *does* run on
every pull request. It installs `kubeconform` and validates every manifest under `k8s/`
(including `k8s/jobs/`) with `-strict`, which does not require a live cluster — only the
manifests' static schema conformance. Marking this job as a required, merge-blocking status
check is a GitHub branch-protection setting outside this repo's code and is tracked as a
manual follow-up.

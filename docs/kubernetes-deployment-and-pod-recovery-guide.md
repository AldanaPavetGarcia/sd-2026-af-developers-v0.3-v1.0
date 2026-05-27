# EOP Deployment and Pod Recovery Guide

This document describes how to build, deploy, and validate the Exodus Ops Platform (EOP) v0.1 server using Docker, Docker Compose, and Kubernetes.

It also documents the local validation workflow for **US-109 · Automatic Pod Recovery**.

---

## 1. Scope

This guide covers:

- building the local EOP server Docker image;
- running the local Compose stack;
- deploying the EOP server to Kubernetes;
- validating liveness and readiness probes;
- validating automatic container restart;
- checking pod logs through `kubectl logs`;
- validating client reconnect and node re-registration after restart.

This guide does not cover:

- full OpenTelemetry span validation from US-110;
- production Helm charts or kustomize overlays;
- persistent registry storage;
- API Gateway, HPC engine, MQTT, or later milestone services.

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
k8s/configmap.yaml
k8s/deployment.yaml
k8s/service.yaml
```

Apply the manifests with:

```bash
kubectl apply -f k8s/
```

Or use the Makefile target:

```bash
make k8s-apply
```

The target waits for the Deployment rollout:

```bash
kubectl rollout status deployment/eop-server --timeout=60s
```

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

---

## 12. CI Policy

The Kubernetes recovery validation is intentionally not part of the normal pull request CI pipeline.

It requires:

- a running Kubernetes cluster;
- `kubectl`;
- a locally available `eop-server:local` image;
- port-forwarding support.

For this reason, it is provided as a reproducible local/manual validation through:

```bash
make k8s-recovery-test
```

The normal CI should remain lightweight and should not depend on minikube or local cluster state.

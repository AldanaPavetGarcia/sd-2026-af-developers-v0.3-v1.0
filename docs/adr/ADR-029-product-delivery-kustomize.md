# ADR-029: Product Delivery — One Kustomize Overlay and One Configuration File

**Status:** Accepted (v1.0, issue #66). Implemented by US-406 (#74).

## Context and Problem

US-406 asks for EOP v1.0 as a product Vault-Tec can launch with one command and no manual steps
afterwards (AC1), with every production setting — ports, retention periods, SLO thresholds — in one
dedicated file separate from code (AC3), Ready in under 3 minutes (AC4), documented and verified
on a clean cluster (AC2), and a `make smoke-test-prod` over the deployed stack (AC8).

What exists after v0.3:

* `make k8s-apply` applies about 25 manifests in `k8s/` in a fixed order, waits for rollouts, and
  runs imperative steps (deleting the pre-canary gateway Deployment, the SigNoz schema Job).
* The gateway is already Kustomize: `k8s/api-gateway/base` and the `canary` / `blue-green`
  overlays (ADR-011), and Loki, Redis, the log agent and the stubs have their own directories.
* Configuration is spread over ConfigMaps, container arguments (Prometheus and Loki retention) and
  provisioned Grafana alert rules (SLO thresholds).
* Pending from v0.3: ZooKeeper and Prometheus have no PersistentVolumeClaim, and ClickHouse's
  768 Mi limit makes it fail background merges, after which SigNoz stores no spans.

## Decision

1. **Kustomize, not Helm.** One overlay, `k8s/overlays/production`, is the product:
   `kubectl apply -k k8s/overlays/production` deploys the whole v1.0 stack. It composes the
   existing directories as bases, so the canary and blue-green procedures of ADR-011 keep working on
   the deployed product.
2. **One configuration file:** `config/production.env` (ports, retention periods, SLO and alert
   thresholds, replica counts, resource sizes). The overlay turns it into ConfigMaps with
   `configMapGenerator`, and Kustomize `replacements` copy values into the places that are not
   environment variables: container arguments (Prometheus `--storage.tsdb.retention.time`, Loki
   retention) and the thresholds in the provisioned Grafana alert rules. Changing a value is an
   edit to that file and the same `kubectl apply -k`.
3. **No ordering by hand.** Dependencies are expressed in the cluster instead of in a script:
   init containers wait for what a Pod needs (ClickHouse before SigNoz and its schema Job, Redis
   before the gateway and the ingestor, the broker before the ingestor), Jobs are idempotent, and
   probes gate traffic. One apply converges with no further steps.
4. **Prerequisites are documented, not steps:** a cluster with an nginx Ingress controller and a
   default StorageClass. On minikube that is `minikube addons enable ingress` and the default
   `standard` class; the README states both before the apply.
5. **Production sizing:** PersistentVolumeClaims for ZooKeeper and Prometheus (retention 35 days,
   needed by US-407's 30-day window) and ClickHouse memory sized for its merges. A `dev`
   overlay keeps the 7 GB-laptop profile (one server replica, memory registry, smaller limits).
6. **Image versions** are pinned in the overlay's `images:` list, so a release is one file to
   review. `make k8s-apply` becomes a thin wrapper around the apply and a readiness wait, and
   `make k8s-ready-time` measures AC4.

## Alternatives Considered

| Alternative | Reason rejected |
|---|---|
| Helm chart | A second packaging system next to the v0.3 Kustomize overlays, a templating language for values the stack does not vary, and a Helm install step on the target cluster; Kustomize ships inside `kubectl`. Helm's release history is not needed: rollbacks go through `kubectl rollout undo` and the canary / blue-green procedures (RUNBOOK) |
| Keep `make k8s-apply` as the product | US-406 AC1 asks for `kubectl apply` with no manual steps; a Makefile with imperative deletes and waits is the manual procedure, automated |
| One ConfigMap edited by hand per service | Fails AC3: settings stay spread over many files and some are not ConfigMaps at all |

## Consequences

* **Positive:** one command, one configuration file, one list of image versions; the same overlays
  serve the demo (canary, blue-green) and the delivery; `kubectl apply -k --dry-run=server` and
  kubeconform validate the whole product in CI.
* **Negative:** `replacements` are verbose and fail silently if a target path changes, so a
  contract test renders the overlay and checks every value from `config/production.env` lands
  where it should. Init containers add a few seconds to cold start, measured against the 3-minute
  budget.

## References

* PRD — US-406; US-407 AC1 (30 days of data).
* ADR-006, ADR-017 — Kubernetes adoption and workload management; ADR-011 — overlays.
* `m3` pending items: ZooKeeper PVC, ClickHouse memory (v0.3 PRs #64, #65).

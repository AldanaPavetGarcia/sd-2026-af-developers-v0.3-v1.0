#!/usr/bin/env bash
set -euo pipefail

# promtool unit tests of the SLO recording rules: extracts eop-slo-rules.yml from the ConfigMap and
# runs tests/prometheus/slo-rules-test.yml with the local promtool, or with the Prometheus image of
# k8s/prometheus-deployment.yaml when promtool is not installed.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="$(sed -nE 's/^ *image: *(prom\/prometheus:[^ ]+)$/\1/p' "${ROOT}/k8s/prometheus-deployment.yaml")"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

python3 - "${ROOT}/k8s/prometheus-rules-configmap.yaml" "${work}/eop-slo-rules.yml" <<'PY'
import sys, yaml
with open(sys.argv[1], encoding="utf-8") as f:
    rules = yaml.safe_load(f)["data"]["eop-slo-rules.yml"]
with open(sys.argv[2], "w", encoding="utf-8") as f:
    f.write(rules)
PY
cp "${ROOT}/tests/prometheus/slo-rules-test.yml" "${work}/"

if command -v promtool >/dev/null; then
    (cd "${work}" && promtool check rules eop-slo-rules.yml && promtool test rules slo-rules-test.yml)
else
    docker run --rm --user "$(id -u):$(id -g)" -v "${work}:/work:ro" -w /work --entrypoint sh "${IMAGE}" \
        -c 'promtool check rules eop-slo-rules.yml && promtool test rules slo-rules-test.yml'
fi

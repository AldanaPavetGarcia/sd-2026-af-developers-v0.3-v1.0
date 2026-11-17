.PHONY: gateway-p99-test gateway-smoke-suite-test blue-green-layout canary-layout blue-green-deploy blue-green-switch blue-green-rollback blue-green-status gateway-smoke-test gateway-overlays-test gateway-load-tool-test canary-deploy canary-rollout canary-rollback canary-promote canary-status gateway-load-test loki-test k8s-log-trace-test deploy-local gateway-test gateway-lint gateway-integration-test gateway-coverage gateway-image k8s-apply k8s-prod-apply k8s-ready-time smoke-test-prod k8s-recovery-test k8s-hpc-batch-demo k8s-hpc-recovery-test k8s-mttr-test edge-certs edge-integration-test k8s-rollout-restart-test slo-run-start slo-run-status slo-run-export observability-test slo-dashboard-test edge-dashboard-test production-config-test edge-test edge-load-tool-test hpc-dashboard-test hpc-metrics-test load-test-hpc prometheus-test benchmark benchmark-report-test tsan vault-tec-dashboard-test sla-export sla-export-tool-test

deploy-local:
	docker compose up --build -d
	./tests/smoke/deploy-local-smoke.sh

k8s-apply:
	# eop-grafana-dashboards isn't a committed YAML — generated straight from
	# monitoring/grafana/dashboards/ so that directory stays the single source of truth for
	# panel content (see k8s/grafana-provisioning-configmap.yaml's top comment).
	kubectl create configmap eop-grafana-dashboards \
		--from-file=monitoring/grafana/dashboards/ \
		--dry-run=client -o yaml | kubectl label --local -f - app=grafana -o yaml | kubectl apply -f -
	# eop-clickhouse-config: same reasoning, generated from monitoring/signoz-common/clickhouse/
	# (config.xml alone is 1000+ lines — see k8s/signoz-clickhouse-deployment.yaml's top comment).
	kubectl create configmap eop-clickhouse-config \
		--from-file=monitoring/signoz-common/clickhouse/ \
		--dry-run=client -o yaml | kubectl label --local -f - app=clickhouse -o yaml | kubectl apply -f -
	# loki-config and otel-log-agent-config: generated from monitoring/ like the dashboards, so
	# those files stay the single source of truth (US-306 AC8).
	kubectl create configmap loki-config --from-file=monitoring/loki/config.yaml \
		--dry-run=client -o yaml | kubectl label --local -f - app=loki -o yaml | kubectl apply -f -
	kubectl create configmap otel-log-agent-config --from-file=monitoring/otel-log-agent/config.yaml \
		--dry-run=client -o yaml | kubectl label --local -f - app=otel-log-agent -o yaml | kubectl apply -f -
	# hpc-engine became a sidecar of eop-server (ADR-027 §3): drop the Deployment of v0.2-v0.3.
	kubectl delete deployment hpc-engine --ignore-not-found
	kubectl apply -f k8s/
	kubectl apply -f k8s/redis/
	# Edge broker and ingestor (US-404): the broker's TLS Secret is generated locally, never committed.
	./scripts/edge-certs.sh --k8s
	kubectl apply -f k8s/mosquitto/
	kubectl apply -f k8s/external-stubs/
	kubectl apply -f k8s/loki/ -f k8s/otel-log-agent/
	# The gateway runs in the canary layout (ADR-011 §4): stable serving, canary at 0 replicas and
	# weight 0. The plain Deployment of v0.3's first releases has a different selector and is
	# replaced by api-gateway-stable.
	kubectl delete deployment api-gateway --ignore-not-found
	kubectl apply -k k8s/api-gateway/overlays/canary
	# 180s, not 60s: US-204 AC6 requires this target to wait up to 3 minutes for all pods to
	# be Ready. 60s already failed live (31/08) — Grafana's 1.4GB image alone took ~70s to
	# pull on a cold node.
	kubectl rollout status deployment/eop-server --timeout=180s
	kubectl rollout status deployment/redis --timeout=180s
	kubectl rollout status statefulset/mosquitto --timeout=180s
	kubectl rollout status deployment/edge-ingestor --timeout=180s
	kubectl rollout status deployment/loki --timeout=180s
	kubectl rollout status daemonset/otel-log-agent --timeout=180s
	kubectl rollout status deployment/ai-service --timeout=180s
	kubectl rollout status deployment/corporate-service --timeout=180s
	kubectl rollout status deployment/api-gateway-stable --timeout=180s
	kubectl rollout status deployment/prometheus --timeout=180s
	kubectl rollout status deployment/grafana --timeout=180s
	# SigNoz stack (US-203 AC5 / US-206 AC5): zookeeper-1 and clickhouse first, since everything
	# else depends on them; the migrator Job must complete before signoz/signoz-otel-collector
	# can do anything useful, but they retry on their own via initContainers/restarts, so it's
	# not a hard gate here.
	kubectl rollout status deployment/zookeeper-1 --timeout=180s
	kubectl rollout status deployment/clickhouse --timeout=180s
	kubectl wait --for=condition=complete job/signoz-telemetrystore-migrator --timeout=180s
	kubectl rollout status deployment/signoz-otel-collector --timeout=180s
	kubectl rollout status deployment/signoz --timeout=180s
	kubectl rollout status deployment/otel-collector --timeout=180s

## EOP v1.0 as a product (US-406, ADR-029): the complete stack from deploy/production, with every
## production value in config/production.env. Needs the images (make gateway-image, eop-server,
## hpc-engine) in the cluster; prerequisites in the README ("Deploy v1.0").
k8s-prod-apply:
	kubectl apply -k deploy/production
	./tests/k8s/wait-ready.sh

## US-406 AC4: apply on a clean cluster and fail if the stack is not Ready within 3 minutes.
k8s-ready-time:
	./tests/k8s/ready-time.sh

## US-406 AC8: server, registration, graph analysis, edge ingestion and gateway health on the
## deployed stack (current kubectl context).
smoke-test-prod:
	python3 tests/k8s/smoke_test_prod.py

## API gateway (US-301, ADR-010). Go module under services/go.
GO_DIR := services/go
GATEWAY_COVERAGE_MIN := 90
EOP_SERVER_BIN ?= $(CURDIR)/build/dev/eop_server
EOP_HPC_ENGINE_BIN ?= $(CURDIR)/build/dev/hpc-engine/hpc_engine

gateway-test:
	cd $(GO_DIR) && go vet ./... && go test -race -count=1 ./...

gateway-lint:
	cd $(GO_DIR) && test -z "$$(gofmt -l .)" && golangci-lint run --build-tags integration ./...

## US-301 AC8 and US-302 AC8: the gateway against a real eop_server and hpc_engine (build both
## first with cmake).
gateway-integration-test:
	cd $(GO_DIR) && EOP_SERVER_BIN=$(EOP_SERVER_BIN) EOP_HPC_ENGINE_BIN=$(EOP_HPC_ENGINE_BIN) \
		go test -race -count=1 -tags integration ./test/integration/

## Fails below GATEWAY_COVERAGE_MIN % of statements (cmd/ only wires packages and is excluded).
gateway-coverage:
	cd $(GO_DIR) && go test -count=1 -coverprofile=coverage.out $$(go list ./... | grep -v /cmd/) >/dev/null \
		&& go tool cover -func=coverage.out | awk -v min=$(GATEWAY_COVERAGE_MIN) \
			'/^total:/ { sub("%", "", $$3); print "gateway coverage: " $$3 "%"; if ($$3 + 0 < min) exit 1 }'

## api-gateway and the external-stub test double (US-303), from the same Dockerfile.
gateway-image:
	docker build -t api-gateway:local $(GO_DIR)
	docker build --target external-stub -t external-stub:local $(GO_DIR)
	docker build --target mttr-watcher -t mttr-watcher:local $(GO_DIR)
	docker build --target edge-ingestor -t edge-ingestor:local $(GO_DIR)
	docker build --target edge-sim -t edge-sim:local $(GO_DIR)
	docker build --target slo-traffic -t slo-traffic:local $(GO_DIR)

## Canary release of the api-gateway (US-304, ADR-011). The Ingress needs ingress-nginx
## (minikube addons enable ingress). Typical run, with make gateway-load-test in another terminal:
##   make canary-deploy IMAGE=api-gateway:0.3.1 VERSION=0.3.1
##   make canary-rollout WEIGHT=10   (then 50, then 100)
##   make canary-promote             or, if the canary misbehaves, make canary-rollback
canary-deploy:
	@test -n "$(IMAGE)" -a -n "$(VERSION)" || { echo "usage: make canary-deploy IMAGE=<image> VERSION=<version>"; exit 1; }
	./scripts/canary.sh deploy "$(IMAGE)" "$(VERSION)"

canary-rollout:
	@test -n "$(WEIGHT)" || { echo "usage: make canary-rollout WEIGHT=<0-100>"; exit 1; }
	./scripts/canary.sh rollout "$(WEIGHT)"

## US-304 AC4: weight 0, timed until no client gets a canary response (< 30 s).
canary-rollback:
	./scripts/canary.sh rollback

canary-promote:
	./scripts/canary.sh promote

canary-status:
	./scripts/canary.sh status

## Continuous load through the Ingress; fails on any 5xx or connection error (US-304 AC6,
## US-305 AC4). Runs until Ctrl-C unless LOAD_ARGS="--duration N".
gateway-load-test:
	python3 tests/load/gateway_load.py $(LOAD_ARGS)

## REST API P99 < 300 ms (v0.3 NFR, ADR-010 §6), measured from the client through the Ingress
## on the read routes; fails over the limit or on any client error.
gateway-p99-test:
	python3 tests/load/gateway_load.py --duration 60 --report-every 15 --max-p99 300 $(LOAD_ARGS)

## Blue-green delivery of the api-gateway (US-305, ADR-011 §3). Move to the blue-green layout
## first (not zero-downtime; carries the running version over), then, with make
## gateway-load-test running in another terminal:
##   make blue-green-deploy IMAGE=api-gateway:0.3.2 VERSION=0.3.2   (goes to the idle slot)
##   make blue-green-switch     smoke tests on the idle slot, then one selector patch
##   make blue-green-rollback   back to the previous slot (< 10 s)
blue-green-layout:
	./scripts/blue-green.sh layout blue-green

canary-layout:
	./scripts/blue-green.sh layout canary

blue-green-deploy:
	@test -n "$(IMAGE)" -a -n "$(VERSION)" || { echo "usage: make blue-green-deploy IMAGE=<image> VERSION=<version>"; exit 1; }
	./scripts/blue-green.sh deploy "$(IMAGE)" "$(VERSION)"

## US-305 AC7: smoke tests and, only if they pass, the switch.
blue-green-switch:
	./scripts/blue-green.sh switch

blue-green-rollback:
	./scripts/blue-green.sh rollback

blue-green-status:
	./scripts/blue-green.sh status

## US-305 AC6 against any gateway URL: make gateway-smoke-test URL=http://localhost:8080
gateway-smoke-test:
	@test -n "$(URL)" || { echo "usage: make gateway-smoke-test URL=<gateway url>"; exit 1; }
	python3 tests/smoke/gateway_smoke.py "$(URL)"

## Static contract of the gateway delivery layouts (ADR-011) and tests of the load tool.
gateway-overlays-test:
	python3 tests/smoke/test_gateway_overlays.py

gateway-load-tool-test:
	python3 tests/load/test_gateway_load.py

gateway-smoke-suite-test:
	python3 tests/smoke/test_gateway_smoke.py

## US-306 AC6 on a running cluster: a failed request has the same trace_id in SigNoz and Loki.
k8s-log-trace-test:
	python3 tests/k8s/log-trace-correlation.py

k8s-recovery-test:
	./tests/k8s/pod-recovery-cycle.sh

## Runs the hpc-engine Job/batch-mode demo (AC5): applies k8s/jobs/ (kept out of
## the k8s/ sweep above since Job specs are immutable) and prints its result.
k8s-hpc-batch-demo:
	kubectl delete job/hpc-engine-batch-demo --ignore-not-found
	kubectl apply -f k8s/jobs/
	kubectl wait --for=condition=complete job/hpc-engine-batch-demo --timeout=60s
	kubectl logs job/hpc-engine-batch-demo

## Self-healing check for the hpc-engine sidecar: its Pod is replaced and the new engine registers
k8s-hpc-recovery-test:
	./tests/k8s/hpc-engine-recovery-cycle.sh

## US-407 AC4 / ADR-027: deletes an eop-server Pod under a 20 ms probe; fails at an MTTR of 200 ms
## or more (needs make k8s-apply and the eop-server, hpc-engine and mttr-watcher images).
## TLS material of the edge broker's device listener (8883) in certs/edge/ (git-ignored).
edge-certs:
	./scripts/edge-certs.sh

## Edge ingestion end to end (US-404 AC8) against a real broker, eop_server and Redis: the
## simulator publishes 10 messages, the ingestor forwards them, the gateway's /v1/nodes shows them.
edge-integration-test:
	cd $(GO_DIR) && EOP_SERVER_BIN=$(EOP_SERVER_BIN) go test -race -count=1 -tags integration -run TestEdge ./test/integration/

k8s-mttr-test:
	./tests/k8s/mttr-cycle.sh

## 30-day SLO run (#67, docs/slo-30-day-run.md): the stack with the persistent Prometheus, plus the
## steady synthetic traffic of k8s/slo-run/ (needs the images of make gateway-image).
slo-run-start: k8s-apply
	kubectl apply -f k8s/slo-run/
	kubectl rollout status deployment/slo-traffic --timeout=120s

## Days collected, traffic flowing, 30-day SLO figures; exits 1 if the run is not collecting.
slo-run-status:
	./tests/k8s/slo-run-status.sh

## End of the run: TSDB snapshot (tar.gz) and hourly SLO series (JSON) into slo-run-export/.
slo-run-export:
	./tests/k8s/slo-run-export.sh

## AC7: rollout restart of eop-server under continuous client load, zero failed operations
k8s-rollout-restart-test:
	./tests/k8s/rollout-restart-under-load.sh

observability-test:
	python3 tests/smoke/test_signoz_integration.py

## US-306: retention as code, log agent pipeline, Loki datasource and request-logs dashboard.
loki-test:
	python3 tests/smoke/test_loki_contract.py

slo-dashboard-test:
	python3 tests/smoke/test_grafana_dashboard_contract.py

## US-405 AC6: 5 minutes of edge-sim against docker-compose, summary checked against Prometheus.
## EDGE_TEST_SECONDS / EDGE_TEST_DEVICES change the load (defaults 300 s, 5 devices).
edge-test:
	docker compose up -d eop-server redis mosquitto edge-ingestor prometheus api-gateway
	python3 tests/smoke/edge_load_test.py

## The comparison logic of edge-test, without the stack.
edge-load-tool-test:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/smoke/test_edge_load_test.py

## US-405: edge dashboard, its P99 alert and their provisioning (static, no Grafana needed).
edge-dashboard-test:
	python3 tests/smoke/test_edge_dashboard_contract.py

## US-407: dashboard JSON, every panel's data as CSV, the headline numbers and a screenshot of
## the SLA period into evidence/ (OUT=…, SLA_DAYS, SLA_STEP). Fails if the error budget is spent.
sla-export:
	./scripts/sla-export.sh $(OUT)

## The export logic of sla-export against a fake Grafana and Prometheus.
sla-export-tool-test:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/smoke/test_export_sla_evidence.py

## US-407: the Vault-Tec delivery dashboard (static, no Grafana needed).
vault-tec-dashboard-test:
	python3 tests/smoke/test_vault_tec_dashboard_contract.py

## US-406 AC1/AC3: config/production.env, the Kustomization listings and the deploy/production render.
production-config-test:
	python3 tests/smoke/test_production_config_contract.py

hpc-dashboard-test:
	python3 tests/smoke/test_hpc_dashboard_contract.py

hpc-metrics-test:
	./build/hpc-engine/hpc_tests --gtest_filter='*HpcMetrics*:*Metrics*'
	python3 tests/smoke/test_hpc_metrics_endpoint.py ./build/hpc-engine/hpc_engine

## Runs local concurrent hpc-engine requests and validates US-206 telemetry fields.
load-test-hpc:
	python3 tests/load/hpc_load_test.py --engine ./build/hpc-engine/hpc_engine

prometheus-test:
	python3 tests/smoke/test_prometheus_config_contract.py

## Tests del análisis del Benchmark Report (Amdahl, AC6, veredictos): no corren el engine
benchmark-report-test:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/smoke/test_benchmark_report.py

## Ejecuta el benchmark completo de OpenMP (Release) y genera BENCHMARK_REPORT.md
benchmark:
	@echo "==> Ejecutando benchmark HPC Engine (Release, sin sanitizers)"
	cd hpc-engine/benchmarks && bash run_benchmark.sh
	@echo "==> Reporte disponible en: hpc-engine/benchmarks/BENCHMARK_REPORT.md"

## Compila y ejecuta los tests HPC bajo ThreadSanitizer (build separada, sin ASAN)
## Compila una build separada (build-tsan) con ThreadSanitizer y corre toda la suite (unit_tests
## + hpc_tests) bajo TSAN -- mismo comando que usa el job "thread-sanitizer" de ci.yml. Antes
## llamaba a run_ci_local.sh, que no existe en el repo (US-202 AC4).
tsan:
	@echo "==> Build TSAN separada para unit_tests + hpc_tests"
	cmake -S . -B build-tsan -G Ninja \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_TOOLCHAIN_FILE=$${VCPKG_ROOT:-$$HOME/vcpkg}/scripts/buildsystems/vcpkg.cmake \
		-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
		-DEXODUS_ENABLE_TSAN=ON -DEXODUS_ENABLE_ASAN=OFF -DEXODUS_ENABLE_COVERAGE=OFF \
		-DEXODUS_BUILD_TESTS=ON -DHPC_BUILD_TESTS=ON
	cmake --build build-tsan --parallel
	TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:ignore_noninstrumented_modules=1" \
		ctest --test-dir build-tsan --output-on-failure --parallel 2
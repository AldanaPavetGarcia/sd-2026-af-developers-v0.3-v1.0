.PHONY: deploy-local gateway-test gateway-lint gateway-integration-test gateway-coverage gateway-image k8s-apply k8s-recovery-test k8s-hpc-batch-demo k8s-hpc-recovery-test k8s-rollout-restart-test observability-test slo-dashboard-test hpc-dashboard-test hpc-metrics-test load-test-hpc prometheus-test benchmark benchmark-report-test tsan

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
	kubectl apply -f k8s/
	kubectl apply -f k8s/redis/
	kubectl apply -f k8s/external-stubs/
	kubectl apply -k k8s/api-gateway/base
	# 180s, not 60s: US-204 AC6 requires this target to wait up to 3 minutes for all pods to
	# be Ready. 60s already failed live (31/08) — Grafana's 1.4GB image alone took ~70s to
	# pull on a cold node.
	kubectl rollout status deployment/eop-server --timeout=180s
	kubectl rollout status deployment/redis --timeout=180s
	kubectl rollout status deployment/ai-service --timeout=180s
	kubectl rollout status deployment/corporate-service --timeout=180s
	kubectl rollout status deployment/api-gateway --timeout=180s
	kubectl rollout status deployment/hpc-engine --timeout=180s
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

k8s-recovery-test:
	./tests/k8s/pod-recovery-cycle.sh

## Runs the hpc-engine Job/batch-mode demo (AC5): applies k8s/jobs/ (kept out of
## the k8s/ sweep above since Job specs are immutable) and prints its result.
k8s-hpc-batch-demo:
	kubectl delete job/hpc-engine-batch-demo --ignore-not-found
	kubectl apply -f k8s/jobs/
	kubectl wait --for=condition=complete job/hpc-engine-batch-demo --timeout=60s
	kubectl logs job/hpc-engine-batch-demo

## Self-healing check for hpc-engine (Deployment equivalent of US-109/k8s-recovery-test)
k8s-hpc-recovery-test:
	./tests/k8s/hpc-engine-recovery-cycle.sh

## AC7: rollout restart of eop-server under continuous client load, zero failed operations
k8s-rollout-restart-test:
	./tests/k8s/rollout-restart-under-load.sh

observability-test:
	python3 tests/smoke/test_signoz_integration.py

slo-dashboard-test:
	python3 tests/smoke/test_grafana_dashboard_contract.py

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
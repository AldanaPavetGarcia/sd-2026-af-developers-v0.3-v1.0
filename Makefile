.PHONY: deploy-local k8s-apply k8s-recovery-test k8s-hpc-batch-demo k8s-hpc-recovery-test k8s-rollout-restart-test observability-test slo-dashboard-test hpc-dashboard-test hpc-metrics-test load-test-hpc prometheus-test benchmark benchmark-report-test tsan firmware-setup firmware-build firmware-fault-build firmware-test firmware-integration-test firmware-tls-build firmware-run firmware-image

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
	# 180s, not 60s: US-204 AC6 requires this target to wait up to 3 minutes for all pods to
	# be Ready. 60s already failed live (31/08) — Grafana's 1.4GB image alone took ~70s to
	# pull on a cold node.
	kubectl rollout status deployment/eop-server --timeout=180s
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

## Edge firmware (US-401..US-403, ADR-014), built for native_sim. make firmware-setup creates
## the Zephyr workspace pinned in firmware/eop-edge/west.yml, outside the repository
## (EOP_ZEPHYR_WS, default ~/.cache/eop-zephyr-ws); the other targets use it.
EOP_ZEPHYR_WS ?= $(HOME)/.cache/eop-zephyr-ws
FIRMWARE_BUILD ?= build/firmware
## Test build with the `eop fault` shell commands (US-403 AC6); never flashed.
FIRMWARE_FAULT_BUILD ?= build/firmware-fault
## Test build with TLS to the broker (tls.conf, ADR-013) and the throwaway CA built into it.
FIRMWARE_TLS_BUILD ?= build/firmware-tls
FIRMWARE_TLS_CA ?= build/firmware-tls-ca
FIRMWARE_BOARD ?= native_sim/native/64
BROKER_HOST ?= 127.0.0.1
## native_sim uses the host compiler; the ESP32 build needs ZEPHYR_TOOLCHAIN=zephyr and the
## Zephyr SDK (firmware/eop-edge/README.md, "ESP32 build").
ZEPHYR_TOOLCHAIN ?= host
ZEPHYR_ENV = . $(EOP_ZEPHYR_WS)/.venv/bin/activate && \
	ZEPHYR_BASE=$(EOP_ZEPHYR_WS)/zephyr ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN)

firmware-setup:
	./scripts/zephyr-setup.sh

firmware-build:
	$(ZEPHYR_ENV) west build -b $(FIRMWARE_BOARD) -d $(FIRMWARE_BUILD) firmware/eop-edge

firmware-fault-build:
	$(ZEPHYR_ENV) west build -b $(FIRMWARE_BOARD) -d $(FIRMWARE_FAULT_BUILD) firmware/eop-edge \
		-- -DEXTRA_CONF_FILE=fault_inject.conf

firmware-tls-build:
	mkdir -p $(FIRMWARE_TLS_CA)
	test -f $(FIRMWARE_TLS_CA)/ca.crt || openssl req -x509 -newkey rsa:2048 -nodes -sha256 \
		-days 30 -keyout $(FIRMWARE_TLS_CA)/ca.key -out $(FIRMWARE_TLS_CA)/ca.crt \
		-subj "/CN=EOP firmware test CA" -addext "basicConstraints=critical,CA:TRUE" \
		-addext "keyUsage=critical,keyCertSign,cRLSign"
	$(ZEPHYR_ENV) west build -b $(FIRMWARE_BOARD) -d $(FIRMWARE_TLS_BUILD) firmware/eop-edge \
		-- -DEXTRA_CONF_FILE=tls.conf -DEOP_MQTT_CA_CERT=$(CURDIR)/$(FIRMWARE_TLS_CA)/ca.crt

## Unit tests (ztest) of backoff, payloads, identity rules, queue, flush, link, watchdog and LED.
firmware-test:
	$(ZEPHYR_ENV) $(EOP_ZEPHYR_WS)/zephyr/scripts/twister -p $(FIRMWARE_BOARD) \
		-T firmware/eop-edge/tests -O build/firmware-twister --inline-logs

## US-401..US-403 against a real Mosquitto in Docker; US-403 uses the fault-injection build and
## the TLS listener the TLS build.
firmware-integration-test: firmware-build firmware-fault-build firmware-tls-build
	python3 firmware/eop-edge/tests/integration/test_publish.py $(FIRMWARE_BUILD)/zephyr/zephyr.exe
	python3 firmware/eop-edge/tests/integration/test_resilience.py \
		$(FIRMWARE_FAULT_BUILD)/zephyr/zephyr.exe
	python3 firmware/eop-edge/tests/integration/test_tls.py $(FIRMWARE_TLS_BUILD)/zephyr/zephyr.exe \
		$(FIRMWARE_TLS_CA)

## One simulated device on this terminal (shell: eop status). FIRMWARE_ARGS="--node-id=vault-7"
firmware-run: firmware-build
	$(FIRMWARE_BUILD)/zephyr/zephyr.exe --broker-host=$(BROKER_HOST) $(FIRMWARE_ARGS)

firmware-image:
	docker build -f firmware/eop-edge/Dockerfile -t eop-edge:local .

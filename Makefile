.PHONY: deploy-local k8s-apply k8s-recovery-test k8s-hpc-batch-demo observability-test benchmark tsan

deploy-local:
	docker compose up --build -d
	./tests/smoke/deploy-local-smoke.sh

k8s-apply:
	kubectl apply -f k8s/
	kubectl rollout status deployment/eop-server --timeout=60s
	kubectl rollout status deployment/hpc-engine --timeout=60s

k8s-recovery-test:
	./tests/k8s/pod-recovery-cycle.sh

## Runs the hpc-engine Job/batch-mode demo (AC5): applies k8s/jobs/ (kept out of
## the k8s/ sweep above since Job specs are immutable) and prints its result.
k8s-hpc-batch-demo:
	kubectl delete job/hpc-engine-batch-demo --ignore-not-found
	kubectl apply -f k8s/jobs/
	kubectl wait --for=condition=complete job/hpc-engine-batch-demo --timeout=60s
	kubectl logs job/hpc-engine-batch-demo

observability-test:
	python3 tests/smoke/test_signoz_integration.py

## Ejecuta el benchmark completo de OpenMP (Release) y genera BENCHMARK_REPORT.md
benchmark:
	@echo "==> Ejecutando benchmark HPC Engine (Release, sin sanitizers)"
	cd hpc-engine/benchmarks && bash run_benchmark.sh
	@echo "==> Reporte disponible en: hpc-engine/benchmarks/BENCHMARK_REPORT.md"

## Compila y ejecuta los tests HPC bajo ThreadSanitizer (build separada, sin ASAN)
tsan:
	@echo "==> Build TSAN separada para hpc_tests"
	ENABLE_TSAN_CHECK=true bash run_ci_local.sh 2>&1 | grep -A5 "PASO 7.5"

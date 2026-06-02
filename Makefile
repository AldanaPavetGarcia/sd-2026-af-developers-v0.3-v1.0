.PHONY: deploy-local k8s-apply k8s-recovery-test observability-test

deploy-local:
	docker compose up --build -d
	./tests/smoke/deploy-local-smoke.sh

k8s-apply:
	kubectl apply -f k8s/
	kubectl rollout status deployment/eop-server --timeout=60s

k8s-recovery-test:
	./tests/k8s/pod-recovery-cycle.sh

observability-test:
	python3 tests/smoke/test_signoz_integration.py

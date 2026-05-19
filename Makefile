.PHONY: deploy-local

deploy-local:
	docker compose up --build -d
	./tests/smoke/deploy-local-smoke.sh

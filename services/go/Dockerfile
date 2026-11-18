# Images of the Go services (ADR-010). Build context: services/go.
#   docker build -t api-gateway:dev services/go
#   docker build --target external-stub -t external-stub:dev services/go
#   docker build --target mttr-watcher -t mttr-watcher:dev services/go

FROM golang:1.27-alpine AS builder
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/api-gateway ./cmd/api-gateway \
 && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/external-stub ./cmd/external-stub \
 && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/mttr-watcher ./cmd/mttr-watcher

# Test double for the external AI and corporate services (ADR-012 §1), kept out of the gateway
# image.
FROM gcr.io/distroless/static-debian12:nonroot AS external-stub
COPY --from=builder /out/external-stub /external-stub
# Numeric UID (distroless "nonroot"), so Kubernetes can verify runAsNonRoot.
USER 65532:65532
EXPOSE 8090
ENTRYPOINT ["/external-stub"]

# eop-server failover measurement (ADR-027 §4): the watcher Deployment and the probe Job of
# make k8s-mttr-test.
FROM gcr.io/distroless/static-debian12:nonroot AS mttr-watcher
COPY --from=builder /out/mttr-watcher /mttr-watcher
USER 65532:65532
EXPOSE 9102
ENTRYPOINT ["/mttr-watcher"]

# Default target: the API gateway. Static binary on distroless: no shell, no package manager,
# non-root user.
FROM gcr.io/distroless/static-debian12:nonroot AS runtime
COPY --from=builder /out/api-gateway /api-gateway
# Numeric UID (distroless "nonroot"), so Kubernetes can verify runAsNonRoot.
USER 65532:65532
EXPOSE 8080 9100
ENTRYPOINT ["/api-gateway"]

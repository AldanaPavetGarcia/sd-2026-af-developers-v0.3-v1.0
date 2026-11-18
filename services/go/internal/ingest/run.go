package ingest

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"github.com/redis/go-redis/v9"
	"go.opentelemetry.io/otel"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

const (
	readHeaderTimeout = 5 * time.Second
	shutdownTimeout   = 5 * time.Second
)

// Run starts the ingestor and serves until ctx ends. metricsListener may be nil (listen on
// cfg.MetricsAddr); tests pass a pre-bound one.
func Run(ctx context.Context, cfg Config, logger *slog.Logger, metricsListener net.Listener) error {
	shutdownTracing, err := telemetry.SetupTracing(ctx, telemetry.TracingConfig{
		Enabled: cfg.TracingEnabled, ServiceName: cfg.ServiceName, Version: cfg.Version, Track: "stable"})
	if err != nil {
		return fmt.Errorf("tracing: %w", err)
	}
	defer func() { _ = shutdownTracing(context.Background()) }()

	client, err := eopclient.New(cfg.Server)
	if err != nil {
		return err
	}
	defer func() { _ = client.Close() }()
	rdb := redis.NewClient(&redis.Options{Addr: cfg.RedisAddr, DB: cfg.RedisDB})
	defer func() { _ = rdb.Close() }()

	reg := prometheus.NewRegistry()
	window := edge.NewWindow(cfg.Window)
	processor := &Processor{
		Prefix: cfg.TopicPrefix, Forward: client, Dedup: RedisDedup{RDB: rdb}, DedupTTL: cfg.DedupTTL,
		Retry: cfg.Retry, Window: window, Metrics: NewMetrics(reg), Tracer: otel.Tracer("edge-ingestor"),
		Logger: logger, Now: time.Now,
	}
	subscriber := &Subscriber{Config: cfg.MQTT, Processor: processor, Logger: logger}

	if metricsListener == nil {
		if metricsListener, err = net.Listen("tcp", cfg.MetricsAddr); err != nil {
			return err
		}
	}
	mux := http.NewServeMux()
	mux.Handle("GET /metrics", promhttp.HandlerFor(reg, promhttp.HandlerOpts{}))
	mux.HandleFunc("GET /livez", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusNoContent) })
	server := &http.Server{Handler: mux, ReadHeaderTimeout: readHeaderTimeout}
	go func() {
		if err := server.Serve(metricsListener); err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("metrics listener failed", "error", err)
		}
	}()
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()

	go PublishStatus(ctx, cfg.StatusInterval, cfg.StatusTTL, cfg.DeviceLiveness, window, subscriber.State,
		func(ctx context.Context, s edge.Snapshot, ttl time.Duration) error {
			return edge.WriteSnapshot(ctx, rdb, s, ttl)
		},
		time.Now, logger)
	logger.Info("edge-ingestor started", "broker", cfg.MQTT.Broker, "eop_server", cfg.Server.Address,
		"redis", cfg.RedisAddr, "metrics", metricsListener.Addr().String())
	err = subscriber.Run(ctx)
	if errors.Is(err, context.Canceled) {
		return nil
	}
	return err
}

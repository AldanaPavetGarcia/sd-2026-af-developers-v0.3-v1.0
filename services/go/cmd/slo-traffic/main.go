// Command slo-traffic is the steady synthetic load of the 30-day SLO run (#67, US-407 AC1/AC2):
// synthetic bunker nodes registered on eop-server with heartbeats, and API reads plus a small
// graph analysis through the gateway. Configuration: EOP_TRAFFIC_* (internal/traffic/config.go).
// Metrics: eop_traffic_* on EOP_TRAFFIC_METRICS_ADDR (:9103).
package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/traffic"
)

const (
	readHeaderTimeout = 5 * time.Second
	shutdownTimeout   = 5 * time.Second
)

func main() {
	logger := telemetry.NewLogger(os.Stdout, "slo-traffic")
	if err := run(logger); err != nil && !errors.Is(err, context.Canceled) {
		logger.Error("slo-traffic stopped with an error", "error", err)
		os.Exit(1)
	}
}

func run(logger *slog.Logger) error {
	cfg, err := traffic.ConfigFromEnv(os.Getenv)
	if err != nil {
		return err
	}
	reg := prometheus.NewRegistry()
	gen := &traffic.Generator{
		Config:  cfg,
		Metrics: traffic.NewMetrics(reg),
		Logger:  logger,
		HTTP:    &http.Client{Timeout: cfg.RequestTimeout},
	}
	mux := http.NewServeMux()
	mux.Handle("/metrics", promhttp.HandlerFor(reg, promhttp.HandlerOpts{}))
	server := &http.Server{Addr: cfg.MetricsAddr, Handler: mux, ReadHeaderTimeout: readHeaderTimeout}
	go func() {
		if err := server.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("metrics listener failed", "error", err)
		}
	}()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, os.Interrupt)
	defer stop()
	logger.Info("slo-traffic started", "gateway", cfg.GatewayURL, "server", cfg.ServerAddr, "nodes", cfg.Nodes,
		"gateway_interval", cfg.GatewayInterval.String(), "analyze_interval", cfg.AnalyzeInterval.String())
	err = gen.Run(ctx)

	shutdownCtx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
	defer cancel()
	_ = server.Shutdown(shutdownCtx)
	return err
}

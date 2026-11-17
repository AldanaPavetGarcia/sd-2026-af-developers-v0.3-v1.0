// Command mttr-watcher measures eop-server's recovery from a Pod failure (ADR-027 §4, US-407 AC4).
//
//	mttr-watcher            watch the eop-server Pods and EndpointSlices; export eop_mttr_* on
//	                        EOP_MTTR_METRICS_ADDR (:9102) for the Grafana MTTR panel
//	mttr-watcher probe      synthetic client for `make k8s-mttr-test`: LIST_NODES through the
//	                        Service every EOP_MTTR_PROBE_INTERVAL (20ms) for EOP_MTTR_PROBE_DURATION,
//	                        then print the outages it saw as one JSON line
//
// Environment: EOP_MTTR_POD_SELECTOR (app=eop-server), EOP_MTTR_SERVICE (eop-server),
// EOP_MTTR_PROBE_ADDR (eop-server:9026), EOP_MTTR_PROBE_TIMEOUT (150ms).
package main

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/mttr"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

const (
	readHeaderTimeout = 5 * time.Second
	shutdownTimeout   = 5 * time.Second
)

func main() {
	logger := telemetry.NewLogger(os.Stdout, "mttr-watcher")
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, os.Interrupt)
	defer stop()

	var err error
	if len(os.Args) > 1 && os.Args[1] == "probe" {
		err = probe(ctx)
	} else {
		err = watch(ctx, logger)
	}
	if err != nil && !errors.Is(err, context.Canceled) {
		logger.Error("mttr-watcher stopped with an error", "error", err)
		os.Exit(1)
	}
}

func watch(ctx context.Context, logger *slog.Logger) error {
	kube, err := mttr.InCluster()
	if err != nil {
		return err
	}
	kube.Logger = logger
	reg := prometheus.NewRegistry()
	watcher := &mttr.Watcher{
		Kube:        kube,
		PodSelector: envOr("EOP_MTTR_POD_SELECTOR", "app=eop-server"),
		Service:     envOr("EOP_MTTR_SERVICE", "eop-server"),
		Metrics:     mttr.NewMetrics(reg),
		Logger:      logger,
		Now:         time.Now,
	}
	mux := http.NewServeMux()
	mux.Handle("/metrics", promhttp.HandlerFor(reg, promhttp.HandlerOpts{}))
	server := &http.Server{Addr: envOr("EOP_MTTR_METRICS_ADDR", ":9102"), Handler: mux, ReadHeaderTimeout: readHeaderTimeout}
	go func() {
		if err := server.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("metrics listener failed", "error", err)
		}
	}()
	logger.Info("mttr-watcher started", "namespace", kube.Namespace, "selector", watcher.PodSelector, "service", watcher.Service)

	err = watcher.Run(ctx)
	shutdownCtx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
	defer cancel()
	_ = server.Shutdown(shutdownCtx)
	return err
}

func probe(ctx context.Context) error {
	cfg := mttr.ProbeConfig{Addr: envOr("EOP_MTTR_PROBE_ADDR", "eop-server:9026")}
	var err error
	if cfg.Interval, err = time.ParseDuration(envOr("EOP_MTTR_PROBE_INTERVAL", "20ms")); err != nil {
		return err
	}
	if cfg.Timeout, err = time.ParseDuration(envOr("EOP_MTTR_PROBE_TIMEOUT", "150ms")); err != nil {
		return err
	}
	duration, err := time.ParseDuration(envOr("EOP_MTTR_PROBE_DURATION", "30s"))
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(ctx, duration)
	defer cancel()
	return json.NewEncoder(os.Stdout).Encode(mttr.Probe(ctx, cfg, time.Now))
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

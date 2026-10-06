// Package app wires the gateway's components and runs its HTTP servers until the context ends.
package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"

	apispec "github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/api"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/api"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/config"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

// Listeners lets tests pass pre-bound sockets; nil means listen on the configured addresses.
type Listeners struct {
	API     net.Listener
	Metrics net.Listener
}

// Run serves the public API and the internal endpoints (/metrics, /livez) until ctx is
// cancelled, then drains in-flight requests within cfg.ShutdownTimeout. A clean shutdown
// returns nil, which main turns into exit code 0.
func Run(ctx context.Context, cfg config.Config, logger *slog.Logger, listeners Listeners) error {
	shutdownTracing, err := telemetry.SetupTracing(ctx, telemetry.TracingConfig{
		Enabled: cfg.TracingEnabled, ServiceName: cfg.ServiceName, Version: cfg.Version, Track: cfg.Track,
	})
	if err != nil {
		return fmt.Errorf("tracing: %w", err)
	}
	defer func() {
		if err := shutdownTracing(context.Background()); err != nil {
			logger.Warn("flushing traces", "error", err)
		}
	}()

	spec, err := apispec.JSON()
	if err != nil {
		return fmt.Errorf("openapi: %w", err)
	}
	client, err := eopclient.New(cfg.Server)
	if err != nil {
		return err
	}
	defer func() { _ = client.Close() }()

	metrics := telemetry.NewMetrics(cfg.Version, cfg.Track)
	apiServer := &http.Server{
		Handler: api.NewServer(api.Options{
			Store: client, Logger: logger, Metrics: metrics, OpenAPIJSON: spec, HealthTimeout: cfg.HealthTimeout,
		}).Routes(),
		ReadHeaderTimeout: cfg.ReadHeaderTimeout,
	}
	internal := http.NewServeMux()
	internal.Handle("GET /metrics", metrics.Handler())
	// Liveness only proves the process serves HTTP; a down eop-server must not restart the
	// gateway, so readiness (/v1/health) is the only probe that checks it (ADR-010 §3).
	internal.HandleFunc("GET /livez", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusNoContent) })
	internalServer := &http.Server{Handler: internal, ReadHeaderTimeout: cfg.ReadHeaderTimeout}

	apiListener, err := listen(listeners.API, cfg.ListenAddr)
	if err != nil {
		return err
	}
	metricsListener, err := listen(listeners.Metrics, cfg.MetricsAddr)
	if err != nil {
		_ = apiListener.Close()
		return err
	}

	errs := make(chan error, 2)
	go func() { errs <- serve(apiServer, apiListener) }()
	go func() { errs <- serve(internalServer, metricsListener) }()
	logger.Info("api-gateway started", "listen", apiListener.Addr().String(),
		"internal", metricsListener.Addr().String(), "eop_server", cfg.Server.Address,
		"version", cfg.Version, "track", cfg.Track)

	var runErr error
	select {
	case <-ctx.Done():
		logger.Info("shutdown requested, draining requests", "timeout", cfg.ShutdownTimeout.String())
	case runErr = <-errs:
		logger.Error("server stopped unexpectedly", "error", runErr)
	}

	shutdownCtx, cancel := context.WithTimeout(context.Background(), cfg.ShutdownTimeout)
	defer cancel()
	return errors.Join(runErr, apiServer.Shutdown(shutdownCtx), internalServer.Shutdown(shutdownCtx))
}

func listen(existing net.Listener, addr string) (net.Listener, error) {
	if existing != nil {
		return existing, nil
	}
	return net.Listen("tcp", addr)
}

func serve(server *http.Server, listener net.Listener) error {
	if err := server.Serve(listener); !errors.Is(err, http.ErrServerClosed) {
		return err
	}
	return nil
}

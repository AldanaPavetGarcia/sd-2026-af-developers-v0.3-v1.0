// Command external-stub plays one of Vault-Tec's external services, the AI service or the
// corporate service, for the API gateway's circuit breaker (ADR-012 §1, US-303).
//
//	EOP_STUB_SERVICE=ai|corporate  EOP_STUB_FAULT=ok|error|slow  EOP_STUB_LISTEN_ADDR=:8090
//	EOP_STUB_SLOW_DELAY=5s
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

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/stub"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

const (
	defaultListenAddr = ":8090"
	defaultSlowDelay  = 5 * time.Second
	shutdownTimeout   = 5 * time.Second
	readHeaderTimeout = 5 * time.Second
)

func main() {
	kind := os.Getenv("EOP_STUB_SERVICE")
	logger := telemetry.NewLogger(os.Stdout, kind+"-service")
	if err := run(kind, logger); err != nil {
		logger.Error("external stub stopped with an error", "error", err)
		os.Exit(1)
	}
}

func run(kind string, logger *slog.Logger) error {
	fault := envOr("EOP_STUB_FAULT", stub.FaultNone)
	delay, err := time.ParseDuration(envOr("EOP_STUB_SLOW_DELAY", defaultSlowDelay.String()))
	if err != nil {
		return err
	}
	s, err := stub.New(kind, fault, delay)
	if err != nil {
		return err
	}
	server := &http.Server{Addr: envOr("EOP_STUB_LISTEN_ADDR", defaultListenAddr), Handler: s.Handler(), ReadHeaderTimeout: readHeaderTimeout}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, os.Interrupt)
	defer stop()
	errs := make(chan error, 1)
	go func() { errs <- server.ListenAndServe() }()
	logger.Info("external stub started", "kind", kind, "listen", server.Addr, "fault", fault)
	select {
	case err := <-errs:
		return err
	case <-ctx.Done():
	}
	shutdownCtx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
	defer cancel()
	if err := server.Shutdown(shutdownCtx); err != nil && !errors.Is(err, http.ErrServerClosed) {
		return err
	}
	return nil
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

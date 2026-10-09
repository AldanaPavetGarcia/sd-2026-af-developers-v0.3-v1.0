// Command api-gateway serves the EOP public REST API (US-301, ADR-010).
package main

import (
	"context"
	"os"
	"os/signal"
	"syscall"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/app"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/config"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

func main() {
	cfg, err := config.Load(os.Getenv)
	logger := telemetry.NewLogger(os.Stdout, cfg.ServiceName)
	if err != nil {
		logger.Error("invalid configuration", "error", err)
		os.Exit(2)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, os.Interrupt)
	defer stop()
	if err := app.Run(ctx, cfg, logger, app.Listeners{}); err != nil {
		logger.Error("api-gateway stopped with an error", "error", err)
		os.Exit(1)
	}
}

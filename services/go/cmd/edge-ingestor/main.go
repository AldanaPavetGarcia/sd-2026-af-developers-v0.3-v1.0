// Command edge-ingestor takes edge device telemetry from the MQTT broker to eop-server
// (US-404, ADR-028). Configuration comes from the environment; see internal/ingest/config.go.
package main

import (
	"context"
	"os"
	"os/signal"
	"syscall"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/ingest"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

func main() {
	logger := telemetry.NewLogger(os.Stdout, "edge-ingestor")
	cfg, err := ingest.Load(os.Getenv)
	if err != nil {
		logger.Error("invalid configuration", "error", err)
		os.Exit(1)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, os.Interrupt)
	defer stop()
	if err := ingest.Run(ctx, cfg, logger, nil); err != nil {
		logger.Error("edge-ingestor stopped with an error", "error", err)
		os.Exit(1)
	}
}

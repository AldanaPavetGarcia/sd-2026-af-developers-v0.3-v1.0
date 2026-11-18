// Command edge-sim plays edge devices with the firmware's payloads (US-404 AC8, US-405).
//
//	EOP_MQTT_BROKER=tcp://mosquitto:1883  EOP_MQTT_TOPIC_PREFIX=eop/bunker
//	EOP_SIM_DEVICES=5  EOP_SIM_NODE_PREFIX=sim-vault-  EOP_SIM_PERIOD=1s
//	EOP_SIM_COUNT=0 (samples per device; 0 runs until stopped)  EOP_SIM_TIMEOUT=5s
//
// With a count it exits 0 once every sample was acknowledged by the broker, and prints the total.
package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/sim"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

func main() {
	logger := telemetry.NewLogger(os.Stdout, "edge-sim")
	cfg, err := load()
	if err != nil {
		logger.Error("invalid configuration", "error", err)
		os.Exit(1)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, os.Interrupt)
	defer stop()
	sent, err := sim.Run(ctx, cfg, sim.MQTTDial(cfg), time.Now, logger)
	logger.Info("simulation finished", "devices", cfg.Devices, "sent", sent)
	if err != nil {
		logger.Error("simulation failed", "error", err)
		os.Exit(1)
	}
}

func load() (sim.Config, error) {
	cfg := sim.Config{
		Broker:      envOr("EOP_MQTT_BROKER", "tcp://127.0.0.1:1883"),
		TopicPrefix: envOr("EOP_MQTT_TOPIC_PREFIX", "eop/bunker"),
		NodePrefix:  envOr("EOP_SIM_NODE_PREFIX", "sim-vault-"),
	}
	var err error
	if cfg.Devices, err = strconv.Atoi(envOr("EOP_SIM_DEVICES", "5")); err != nil || cfg.Devices <= 0 {
		return cfg, fmt.Errorf("EOP_SIM_DEVICES must be a positive integer")
	}
	if cfg.Count, err = strconv.Atoi(envOr("EOP_SIM_COUNT", "0")); err != nil || cfg.Count < 0 {
		return cfg, fmt.Errorf("EOP_SIM_COUNT must be 0 or a positive integer")
	}
	if cfg.Period, err = time.ParseDuration(envOr("EOP_SIM_PERIOD", "1s")); err != nil || cfg.Period <= 0 {
		return cfg, fmt.Errorf("EOP_SIM_PERIOD must be a positive duration")
	}
	if cfg.Timeout, err = time.ParseDuration(envOr("EOP_SIM_TIMEOUT", "5s")); err != nil || cfg.Timeout <= 0 {
		return cfg, fmt.Errorf("EOP_SIM_TIMEOUT must be a positive duration")
	}
	return cfg, nil
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

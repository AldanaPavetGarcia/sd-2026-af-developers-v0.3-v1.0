package config

import (
	"strings"
	"testing"
	"time"
)

func env(values map[string]string) func(string) string {
	return func(key string) string { return values[key] }
}

func TestLoadDefaults(t *testing.T) {
	cfg, err := Load(env(nil))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if cfg.ListenAddr != DefaultListenAddr || cfg.Server.Address != DefaultServerAddr ||
		cfg.Server.IdleTimeout != DefaultPoolIdleTimeout || cfg.Server.MaxConns != DefaultPoolSize ||
		cfg.ServiceName != DefaultServiceName || cfg.Track != DefaultTrack || !cfg.TracingEnabled {
		t.Fatalf("unexpected defaults: %+v", cfg)
	}
}

func TestLoadOverrides(t *testing.T) {
	cfg, err := Load(env(map[string]string{
		"EOP_SERVER_ADDR":              "eop-server:9026",
		"EOP_SERVER_POOL_IDLE_TIMEOUT": "10s",
		"EOP_SERVER_POOL_SIZE":         "4",
		"EOP_GATEWAY_VERSION":          "1.2.0",
		"EOP_GATEWAY_TRACK":            "canary",
		"EOP_TRACING_ENABLED":          "0",
	}))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if cfg.Server.Address != "eop-server:9026" || cfg.Server.IdleTimeout != 10*time.Second ||
		cfg.Server.MaxConns != 4 || cfg.Version != "1.2.0" || cfg.Track != "canary" || cfg.TracingEnabled {
		t.Fatalf("overrides not applied: %+v", cfg)
	}
}

func TestLoadRejectsInvalidValues(t *testing.T) {
	tests := map[string]map[string]string{
		"duration":         {"EOP_SERVER_DIAL_TIMEOUT": "two seconds"},
		"integer":          {"EOP_SERVER_POOL_SIZE": "many"},
		"boolean":          {"EOP_TRACING_ENABLED": "yes"},
		"non-positive":     {"EOP_SERVER_POOL_SIZE": "0"},
		"gateway timeout":  {"EOP_GATEWAY_HEALTH_TIMEOUT": "0s"},
		"negative timeout": {"EOP_SERVER_REQUEST_TIMEOUT": "-1s"},
	}
	for name, values := range tests {
		t.Run(name, func(t *testing.T) {
			if _, err := Load(env(values)); err == nil {
				t.Fatalf("Load accepted %v", values)
			}
		})
	}
}

func TestLoadReportsTheOffendingKey(t *testing.T) {
	_, err := Load(env(map[string]string{"EOP_SERVER_POOL_SIZE": "x", "EOP_SERVER_DIAL_TIMEOUT": "y"}))
	if err == nil || !strings.Contains(err.Error(), "EOP_SERVER_") {
		t.Fatalf("error = %v, want the key named", err)
	}
}

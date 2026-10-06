// Package config reads the gateway configuration from the environment. Every value has a
// documented default so a local run needs no setup, and every value can be overridden by the
// Kubernetes ConfigMap.
package config

import (
	"errors"
	"fmt"
	"strconv"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
)

// Defaults. The pool idle timeout stays below the server's EOP_IDLE_TIMEOUT (30 s in
// k8s/configmap.yaml) and the pool size well below EOP_MAX_CLIENTS (100).
const (
	DefaultListenAddr        = ":8080"
	DefaultMetricsAddr       = ":9100"
	DefaultServerAddr        = "127.0.0.1:9026"
	DefaultDialTimeout       = 2 * time.Second
	DefaultRequestTimeout    = 3 * time.Second
	DefaultPoolIdleTimeout   = 20 * time.Second
	DefaultPoolSize          = 8
	DefaultMaxListNodes      = 100000
	DefaultMaxLineBytes      = 64 * 1024
	DefaultHealthTimeout     = time.Second
	DefaultShutdownTimeout   = 15 * time.Second
	DefaultReadHeaderTimeout = 5 * time.Second
	DefaultServiceName       = "api-gateway"
	DefaultVersion           = "dev"
	DefaultTrack             = "stable"
)

// Config is the complete gateway configuration.
type Config struct {
	ListenAddr        string
	MetricsAddr       string
	Server            eopclient.Options
	HealthTimeout     time.Duration
	ShutdownTimeout   time.Duration
	ReadHeaderTimeout time.Duration
	ServiceName       string
	Version           string
	Track             string
	TracingEnabled    bool
}

// Load reads the configuration through getenv (os.Getenv in production).
func Load(getenv func(string) string) (Config, error) {
	r := reader{getenv: getenv}
	cfg := Config{
		ListenAddr:  r.str("EOP_GATEWAY_LISTEN_ADDR", DefaultListenAddr),
		MetricsAddr: r.str("EOP_GATEWAY_METRICS_ADDR", DefaultMetricsAddr),
		Server: eopclient.Options{
			Address:        r.str("EOP_SERVER_ADDR", DefaultServerAddr),
			DialTimeout:    r.duration("EOP_SERVER_DIAL_TIMEOUT", DefaultDialTimeout),
			RequestTimeout: r.duration("EOP_SERVER_REQUEST_TIMEOUT", DefaultRequestTimeout),
			IdleTimeout:    r.duration("EOP_SERVER_POOL_IDLE_TIMEOUT", DefaultPoolIdleTimeout),
			MaxConns:       r.integer("EOP_SERVER_POOL_SIZE", DefaultPoolSize),
			MaxListNodes:   r.integer("EOP_GATEWAY_MAX_LIST_NODES", DefaultMaxListNodes),
			MaxLineBytes:   r.integer("EOP_GATEWAY_MAX_LINE_BYTES", DefaultMaxLineBytes),
		},
		HealthTimeout:     r.duration("EOP_GATEWAY_HEALTH_TIMEOUT", DefaultHealthTimeout),
		ShutdownTimeout:   r.duration("EOP_GATEWAY_SHUTDOWN_TIMEOUT", DefaultShutdownTimeout),
		ReadHeaderTimeout: r.duration("EOP_GATEWAY_READ_HEADER_TIMEOUT", DefaultReadHeaderTimeout),
		ServiceName:       r.str("OTEL_SERVICE_NAME", DefaultServiceName),
		Version:           r.str("EOP_GATEWAY_VERSION", DefaultVersion),
		Track:             r.str("EOP_GATEWAY_TRACK", DefaultTrack),
		TracingEnabled:    r.boolean("EOP_TRACING_ENABLED", true),
	}
	if r.err != nil {
		return Config{}, r.err
	}
	if err := cfg.Server.Validate(); err != nil {
		return Config{}, err
	}
	if cfg.HealthTimeout <= 0 || cfg.ShutdownTimeout <= 0 || cfg.ReadHeaderTimeout <= 0 {
		return Config{}, errors.New("config: gateway timeouts must be positive")
	}
	return cfg, nil
}

// reader keeps the first parse error so Load can report it once.
type reader struct {
	getenv func(string) string
	err    error
}

func (r *reader) str(key, fallback string) string {
	if v := r.getenv(key); v != "" {
		return v
	}
	return fallback
}

func (r *reader) duration(key string, fallback time.Duration) time.Duration {
	v := r.getenv(key)
	if v == "" {
		return fallback
	}
	d, err := time.ParseDuration(v)
	if err != nil {
		r.fail(key, v, "a duration such as 2s or 500ms")
		return fallback
	}
	return d
}

func (r *reader) integer(key string, fallback int) int {
	v := r.getenv(key)
	if v == "" {
		return fallback
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		r.fail(key, v, "an integer")
		return fallback
	}
	return n
}

func (r *reader) boolean(key string, fallback bool) bool {
	switch r.getenv(key) {
	case "":
		return fallback
	case "1", "true":
		return true
	case "0", "false":
		return false
	default:
		r.fail(key, r.getenv(key), "1, 0, true or false")
		return fallback
	}
}

func (r *reader) fail(key, value, expected string) {
	if r.err == nil {
		r.err = fmt.Errorf("config: %s=%q is not %s", key, value, expected)
	}
}

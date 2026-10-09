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

	// Graph analysis (US-302). The engine answers within the server's EOP_HPC_TIMEOUT_MS (5 s by
	// default, at most 60 s), so the gateway waits a little longer than the largest value.
	DefaultAnalyzeTimeout          = 65 * time.Second
	DefaultMaxAnalyzeResponseBytes = 64 * 1024 * 1024
	DefaultMaxBodyBytes            = 32 * 1024 * 1024
	DefaultAsyncMinNodes           = 1000
	DefaultSyncAnalyzeTimeout      = 5 * time.Second
	DefaultJobWorkers              = 1
	DefaultJobQueue                = 16
	DefaultJobResultTTL            = 5 * time.Minute
	DefaultJobMaxAge               = 30 * time.Minute
	DefaultRedisAddr               = "127.0.0.1:6379"
	DefaultVersion                 = "dev"
	DefaultTrack                   = "stable"
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
	Analysis          Analysis
	RedisAddr         string
	RedisDB           int
}

// Analysis configures graph analysis requests and asynchronous jobs (US-302).
type Analysis struct {
	// MaxBodyBytes caps a request body; it stays below eop-server's 64 MiB framing cap.
	MaxBodyBytes int
	// AsyncMinNodes is the graph size from which a request becomes a job (US-302 AC2).
	AsyncMinNodes int
	// SyncTimeout bounds a synchronous analysis (US-302 AC1: within 5 s).
	SyncTimeout time.Duration
	// JobWorkers run jobs concurrently in this pod; the engine serializes them anyway.
	JobWorkers int
	// JobQueue is how many accepted jobs may wait for a worker before new ones are refused.
	JobQueue int
	// JobResultTTL keeps a finished job readable (US-302 AC5: at least 5 minutes).
	JobResultTTL time.Duration
	// JobMaxAge expires a job that never finished, for example because its pod died.
	JobMaxAge time.Duration
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

			AnalyzeTimeout:          r.duration("EOP_SERVER_ANALYZE_TIMEOUT", DefaultAnalyzeTimeout),
			MaxAnalyzeResponseBytes: r.integer("EOP_GATEWAY_MAX_ANALYZE_RESPONSE_BYTES", DefaultMaxAnalyzeResponseBytes),
		},
		Analysis: Analysis{
			MaxBodyBytes:  r.integer("EOP_GATEWAY_MAX_BODY_BYTES", DefaultMaxBodyBytes),
			AsyncMinNodes: r.integer("EOP_GATEWAY_ASYNC_MIN_NODES", DefaultAsyncMinNodes),
			SyncTimeout:   r.duration("EOP_GATEWAY_SYNC_ANALYZE_TIMEOUT", DefaultSyncAnalyzeTimeout),
			JobWorkers:    r.integer("EOP_GATEWAY_JOB_WORKERS", DefaultJobWorkers),
			JobQueue:      r.integer("EOP_GATEWAY_JOB_QUEUE", DefaultJobQueue),
			JobResultTTL:  r.duration("EOP_GATEWAY_JOB_RESULT_TTL", DefaultJobResultTTL),
			JobMaxAge:     r.duration("EOP_GATEWAY_JOB_MAX_AGE", DefaultJobMaxAge),
		},
		RedisAddr:         r.str("EOP_REDIS_ADDR", DefaultRedisAddr),
		RedisDB:           r.integer("EOP_REDIS_DB", 0),
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
	if err := cfg.Analysis.validate(); err != nil {
		return Config{}, err
	}
	if cfg.RedisDB < 0 {
		return Config{}, errors.New("config: EOP_REDIS_DB must not be negative")
	}
	return cfg, nil
}

func (a Analysis) validate() error {
	switch {
	case a.MaxBodyBytes <= 0, a.AsyncMinNodes <= 0, a.JobWorkers <= 0, a.JobQueue <= 0:
		return errors.New("config: analysis limits must be positive")
	case a.SyncTimeout <= 0, a.JobResultTTL <= 0:
		return errors.New("config: analysis timeouts must be positive")
	case a.JobMaxAge < a.JobResultTTL:
		return errors.New("config: EOP_GATEWAY_JOB_MAX_AGE must not be shorter than EOP_GATEWAY_JOB_RESULT_TTL")
	}
	return nil
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

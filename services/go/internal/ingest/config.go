package ingest

import (
	"errors"
	"fmt"
	"strconv"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
)

// Defaults of the edge ingestor (ADR-028); every one is overridden by an environment variable.
const (
	DefaultBroker               = "tcp://127.0.0.1:1883"
	DefaultClientID             = "eop-edge-ingestor"
	DefaultTopicPrefix          = "eop/bunker"
	DefaultKeepAlive            = 30 * time.Second
	DefaultConnectTimeout       = 5 * time.Second
	DefaultMaxReconnectInterval = 30 * time.Second
	DefaultQueue                = 256
	DefaultServerAddr           = "127.0.0.1:9026"
	DefaultRedisAddr            = "127.0.0.1:6379"
	DefaultDedupTTL             = 5 * time.Minute
	DefaultRetryInitial         = 100 * time.Millisecond
	DefaultRetryMax             = 5 * time.Second
	DefaultStatusInterval       = time.Second
	DefaultStatusTTL            = 5 * time.Second
	DefaultWindow               = time.Minute
	DefaultDeviceLiveness       = 10 * time.Second
	DefaultMetricsAddr          = ":9103"
	DefaultServiceName          = "edge-ingestor"
	DefaultVersion              = "dev"

	serverDialTimeout    = 2 * time.Second
	serverRequestTimeout = 3 * time.Second
	serverIdleTimeout    = 20 * time.Second
	serverPoolSize       = 2
	serverMaxLineBytes   = 64 * 1024
)

// Config is the edge ingestor's configuration.
type Config struct {
	MQTT           MQTTConfig
	TopicPrefix    string
	Server         eopclient.Options
	RedisAddr      string
	RedisDB        int
	DedupTTL       time.Duration
	Retry          Backoff
	StatusInterval time.Duration
	StatusTTL      time.Duration
	Window         time.Duration
	DeviceLiveness time.Duration
	MetricsAddr    string
	ServiceName    string
	Version        string
	TracingEnabled bool
}

// Load reads the configuration through getenv (os.Getenv in production).
func Load(getenv func(string) string) (Config, error) {
	r := reader{getenv: getenv}
	cfg := Config{
		MQTT: MQTTConfig{
			Broker:               r.str("EOP_MQTT_BROKER", DefaultBroker),
			ClientID:             r.str("EOP_MQTT_CLIENT_ID", DefaultClientID),
			Username:             getenv("EOP_MQTT_USERNAME"),
			Password:             getenv("EOP_MQTT_PASSWORD"),
			KeepAlive:            r.duration("EOP_MQTT_KEEPALIVE", DefaultKeepAlive),
			ConnectTimeout:       r.duration("EOP_MQTT_CONNECT_TIMEOUT", DefaultConnectTimeout),
			MaxReconnectInterval: r.duration("EOP_MQTT_MAX_RECONNECT_INTERVAL", DefaultMaxReconnectInterval),
			Queue:                r.integer("EOP_EDGE_QUEUE", DefaultQueue),
		},
		TopicPrefix: r.str("EOP_MQTT_TOPIC_PREFIX", DefaultTopicPrefix),
		Server: eopclient.Options{
			Address:        r.str("EOP_SERVER_ADDR", DefaultServerAddr),
			DialTimeout:    serverDialTimeout,
			RequestTimeout: r.duration("EOP_SERVER_REQUEST_TIMEOUT", serverRequestTimeout),
			IdleTimeout:    serverIdleTimeout,
			MaxConns:       serverPoolSize,
			MaxListNodes:   1,
			MaxLineBytes:   serverMaxLineBytes,
			// The ingestor never sends ANALYZE_GRAPH; the client still requires positive limits.
			AnalyzeTimeout:          serverRequestTimeout,
			MaxAnalyzeResponseBytes: serverMaxLineBytes,
		},
		RedisAddr:      r.str("EOP_REDIS_ADDR", DefaultRedisAddr),
		RedisDB:        r.integer("EOP_REDIS_DB", 0),
		DedupTTL:       r.duration("EOP_EDGE_DEDUP_TTL", DefaultDedupTTL),
		Retry:          Backoff{Initial: r.duration("EOP_EDGE_RETRY_INITIAL", DefaultRetryInitial), Max: r.duration("EOP_EDGE_RETRY_MAX", DefaultRetryMax)},
		StatusInterval: r.duration("EOP_EDGE_STATUS_INTERVAL", DefaultStatusInterval),
		StatusTTL:      r.duration("EOP_EDGE_STATUS_TTL", DefaultStatusTTL),
		Window:         r.duration("EOP_EDGE_WINDOW", DefaultWindow),
		DeviceLiveness: r.duration("EOP_EDGE_DEVICE_LIVENESS", DefaultDeviceLiveness),
		MetricsAddr:    r.str("EOP_EDGE_METRICS_ADDR", DefaultMetricsAddr),
		ServiceName:    r.str("OTEL_SERVICE_NAME", DefaultServiceName),
		Version:        r.str("EOP_EDGE_VERSION", DefaultVersion),
		TracingEnabled: r.boolean("EOP_TRACING_ENABLED", true),
	}
	if r.err != nil {
		return Config{}, r.err
	}
	if err := cfg.Server.Validate(); err != nil {
		return Config{}, err
	}
	switch {
	case cfg.MQTT.KeepAlive <= 0, cfg.MQTT.ConnectTimeout <= 0, cfg.MQTT.MaxReconnectInterval <= 0,
		cfg.DedupTTL <= 0, cfg.Retry.Initial <= 0, cfg.StatusInterval <= 0, cfg.Window <= 0, cfg.DeviceLiveness <= 0:
		return Config{}, errors.New("ingest: durations must be positive")
	case cfg.Retry.Max < cfg.Retry.Initial:
		return Config{}, errors.New("ingest: EOP_EDGE_RETRY_MAX must not be below EOP_EDGE_RETRY_INITIAL")
	case cfg.StatusTTL <= cfg.StatusInterval:
		return Config{}, errors.New("ingest: EOP_EDGE_STATUS_TTL must be longer than EOP_EDGE_STATUS_INTERVAL")
	case cfg.MQTT.Queue <= 0:
		return Config{}, errors.New("ingest: EOP_EDGE_QUEUE must be positive")
	case cfg.RedisDB < 0:
		return Config{}, errors.New("ingest: EOP_REDIS_DB must not be negative")
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

func (r *reader) fail(key, value, want string) {
	if r.err == nil {
		r.err = fmt.Errorf("ingest: %s=%q must be %s", key, value, want)
	}
}

// Package traffic is the steady synthetic load of the 30-day SLO run (#67, US-407 AC1/AC2): a few
// bunker nodes that stay registered on eop-server with heartbeats, and an API client that reads
// the node state through the gateway and asks for a small graph analysis now and then. It keeps
// the SLO dashboard fed with real requests at a low, constant rate; every outcome is a metric and
// only failures and a periodic summary are logged, so it adds little to logs and traces.
package traffic

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

// Config is the whole load, from the environment (ConfigFromEnv).
type Config struct {
	GatewayURL        string        // EOP_TRAFFIC_GATEWAY_URL, http://api-gateway
	GatewayPaths      []string      // EOP_TRAFFIC_GATEWAY_PATHS, requested in turn
	GatewayInterval   time.Duration // EOP_TRAFFIC_GATEWAY_INTERVAL, between gateway requests
	AnalyzeInterval   time.Duration // EOP_TRAFFIC_ANALYZE_INTERVAL, 0 disables
	ServerAddr        string        // EOP_TRAFFIC_SERVER_ADDR, eop-server:9026
	Nodes             int           // EOP_TRAFFIC_NODES, synthetic bunkers kept registered
	NodePrefix        string        // EOP_TRAFFIC_NODE_PREFIX, node_id = <prefix><n>
	HeartbeatInterval time.Duration // EOP_TRAFFIC_HEARTBEAT_INTERVAL, below 2 × EOP_HEARTBEAT_INTERVAL
	RequestTimeout    time.Duration // EOP_TRAFFIC_REQUEST_TIMEOUT
	SummaryEvery      time.Duration // EOP_TRAFFIC_SUMMARY_EVERY, one log line per period
	MetricsAddr       string        // EOP_TRAFFIC_METRICS_ADDR
}

// Defaults: about 1.3 requests per second in total, ~110,000 a day.
func defaults() Config {
	return Config{
		GatewayURL:        "http://api-gateway",
		GatewayPaths:      []string{"/v1/health", "/v1/nodes", "/v1/nodes/{node}"},
		GatewayInterval:   time.Second,
		AnalyzeInterval:   5 * time.Minute,
		ServerAddr:        "eop-server:9026",
		Nodes:             3,
		NodePrefix:        "slo-bunker-",
		HeartbeatInterval: 5 * time.Second,
		RequestTimeout:    5 * time.Second,
		SummaryEvery:      10 * time.Minute,
		MetricsAddr:       ":9103",
	}
}

// ConfigFromEnv reads the configuration; unset variables keep their defaults.
func ConfigFromEnv(getenv func(string) string) (Config, error) {
	cfg := defaults()
	str := func(name string, dst *string) {
		if v := strings.TrimSpace(getenv(name)); v != "" {
			*dst = v
		}
	}
	var errs []error
	dur := func(name string, dst *time.Duration, allowZero bool) {
		v := strings.TrimSpace(getenv(name))
		if v == "" {
			return
		}
		d, err := time.ParseDuration(v)
		if err != nil || d < 0 || (d == 0 && !allowZero) {
			errs = append(errs, fmt.Errorf("%s: %q is not a valid duration", name, v))
			return
		}
		*dst = d
	}

	str("EOP_TRAFFIC_GATEWAY_URL", &cfg.GatewayURL)
	if v := strings.TrimSpace(getenv("EOP_TRAFFIC_GATEWAY_PATHS")); v != "" {
		cfg.GatewayPaths = strings.Split(v, ",")
	}
	dur("EOP_TRAFFIC_GATEWAY_INTERVAL", &cfg.GatewayInterval, false)
	dur("EOP_TRAFFIC_ANALYZE_INTERVAL", &cfg.AnalyzeInterval, true)
	str("EOP_TRAFFIC_SERVER_ADDR", &cfg.ServerAddr)
	if v := strings.TrimSpace(getenv("EOP_TRAFFIC_NODES")); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 {
			errs = append(errs, fmt.Errorf("EOP_TRAFFIC_NODES: %q is not a non-negative integer", v))
		} else {
			cfg.Nodes = n
		}
	}
	str("EOP_TRAFFIC_NODE_PREFIX", &cfg.NodePrefix)
	dur("EOP_TRAFFIC_HEARTBEAT_INTERVAL", &cfg.HeartbeatInterval, false)
	dur("EOP_TRAFFIC_REQUEST_TIMEOUT", &cfg.RequestTimeout, false)
	dur("EOP_TRAFFIC_SUMMARY_EVERY", &cfg.SummaryEvery, false)
	str("EOP_TRAFFIC_METRICS_ADDR", &cfg.MetricsAddr)

	for i, p := range cfg.GatewayPaths {
		cfg.GatewayPaths[i] = strings.TrimSpace(p)
		if !strings.HasPrefix(cfg.GatewayPaths[i], "/") {
			errs = append(errs, fmt.Errorf("EOP_TRAFFIC_GATEWAY_PATHS: %q must start with /", p))
		}
	}
	return cfg, errors.Join(errs...)
}

// NodeID is the node_id of synthetic bunker n (1-based).
func (c Config) NodeID(n int) string {
	return c.NodePrefix + strconv.Itoa(n)
}

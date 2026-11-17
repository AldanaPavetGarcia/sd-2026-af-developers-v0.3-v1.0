package traffic

import (
	"strings"
	"testing"
	"time"
)

func envFrom(m map[string]string) func(string) string {
	return func(k string) string { return m[k] }
}

func TestDefaults(t *testing.T) {
	cfg, err := ConfigFromEnv(envFrom(nil))
	if err != nil {
		t.Fatal(err)
	}
	if cfg.GatewayURL != "http://api-gateway" || cfg.ServerAddr != "eop-server:9026" || cfg.Nodes != 3 ||
		cfg.GatewayInterval != time.Second || cfg.AnalyzeInterval != 5*time.Minute || cfg.MetricsAddr != ":9103" {
		t.Fatalf("%+v", cfg)
	}
	if cfg.NodeID(2) != "slo-bunker-2" {
		t.Fatal(cfg.NodeID(2))
	}
}

func TestOverrides(t *testing.T) {
	cfg, err := ConfigFromEnv(envFrom(map[string]string{
		"EOP_TRAFFIC_GATEWAY_URL":        "http://gw:8080",
		"EOP_TRAFFIC_GATEWAY_PATHS":      "/v1/health, /v1/nodes",
		"EOP_TRAFFIC_GATEWAY_INTERVAL":   "250ms",
		"EOP_TRAFFIC_ANALYZE_INTERVAL":   "0",
		"EOP_TRAFFIC_SERVER_ADDR":        "srv:1",
		"EOP_TRAFFIC_NODES":              "0",
		"EOP_TRAFFIC_NODE_PREFIX":        "x-",
		"EOP_TRAFFIC_HEARTBEAT_INTERVAL": "2s",
		"EOP_TRAFFIC_REQUEST_TIMEOUT":    "1s",
		"EOP_TRAFFIC_SUMMARY_EVERY":      "1h",
		"EOP_TRAFFIC_METRICS_ADDR":       ":1",
	}))
	if err != nil {
		t.Fatal(err)
	}
	if cfg.GatewayURL != "http://gw:8080" || strings.Join(cfg.GatewayPaths, "|") != "/v1/health|/v1/nodes" ||
		cfg.GatewayInterval != 250*time.Millisecond || cfg.AnalyzeInterval != 0 || cfg.ServerAddr != "srv:1" ||
		cfg.Nodes != 0 || cfg.NodePrefix != "x-" || cfg.HeartbeatInterval != 2*time.Second ||
		cfg.RequestTimeout != time.Second || cfg.SummaryEvery != time.Hour || cfg.MetricsAddr != ":1" {
		t.Fatalf("%+v", cfg)
	}
}

func TestInvalidValuesAreAllReported(t *testing.T) {
	_, err := ConfigFromEnv(envFrom(map[string]string{
		"EOP_TRAFFIC_GATEWAY_INTERVAL": "0",
		"EOP_TRAFFIC_ANALYZE_INTERVAL": "-1s",
		"EOP_TRAFFIC_NODES":            "many",
		"EOP_TRAFFIC_GATEWAY_PATHS":    "v1/health",
		"EOP_TRAFFIC_REQUEST_TIMEOUT":  "soon",
	}))
	if err == nil {
		t.Fatal("accepted")
	}
	for _, name := range []string{"GATEWAY_INTERVAL", "ANALYZE_INTERVAL", "EOP_TRAFFIC_NODES", "GATEWAY_PATHS", "REQUEST_TIMEOUT"} {
		if !strings.Contains(err.Error(), name) {
			t.Errorf("%s not reported in %v", name, err)
		}
	}
}

//go:build integration

package integration

import (
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/app"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/config"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/stub"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

// restartable is an external service that can be stopped and started on the same address,
// like scaling its Deployment to 0 and back.
type restartable struct {
	t       *testing.T
	addr    string
	handler http.Handler
	server  *http.Server
}

func startRestartable(t *testing.T, kind string) *restartable {
	t.Helper()
	s, err := stub.New(kind, stub.FaultNone, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	r := &restartable{t: t, handler: s.Handler()}
	r.start("127.0.0.1:0")
	t.Cleanup(r.stop)
	return r
}

func (r *restartable) start(addr string) {
	r.t.Helper()
	l, err := net.Listen("tcp", addr)
	if err != nil {
		r.t.Fatal(err)
	}
	r.addr = l.Addr().String()
	r.server = &http.Server{Handler: r.handler, ReadHeaderTimeout: time.Second}
	go func() { _ = r.server.Serve(l) }()
}

func (r *restartable) stop() {
	if r.server != nil {
		_ = r.server.Close()
		r.server = nil
	}
}

func breakerGateway(t *testing.T, serverAddr, aiAddr, corpAddr, settingsPath string) (apiURL, metricsURL string) {
	t.Helper()
	cfg, err := config.Load(func(key string) string {
		return map[string]string{
			"EOP_SERVER_ADDR": serverAddr, "EOP_REDIS_ADDR": miniredis.RunT(t).Addr(), "EOP_TRACING_ENABLED": "0",
			"EOP_AI_SERVICE_URL": "http://" + aiAddr, "EOP_CORPORATE_SERVICE_URL": "http://" + corpAddr,
			"EOP_GATEWAY_BREAKER_SETTINGS": settingsPath, "EOP_GATEWAY_BREAKER_RELOAD_INTERVAL": "50ms",
		}[key]
	})
	if err != nil {
		t.Fatal(err)
	}
	apiL, _ := net.Listen("tcp", "127.0.0.1:0")
	internalL, _ := net.Listen("tcp", "127.0.0.1:0")
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- app.Run(ctx, cfg, telemetry.NewLogger(io.Discard, "api-gateway"), app.Listeners{API: apiL, Metrics: internalL})
	}()
	t.Cleanup(func() {
		cancel()
		if err := <-done; err != nil {
			t.Errorf("gateway shutdown: %v", err)
		}
	})
	return "http://" + apiL.Addr().String(), "http://" + internalL.Addr().String()
}

func getEnvelope(t *testing.T, url string) (int, map[string]any, http.Header) {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = resp.Body.Close() }()
	var body map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, body, resp.Header
}

func metricsText(t *testing.T, url string) string {
	t.Helper()
	resp, err := http.Get(url + "/metrics")
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = resp.Body.Close() }()
	b, _ := io.ReadAll(resp.Body)
	return string(b)
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

// TestCircuitBreakerAgainstAStoppedService is US-303 AC7: external service goes down → circuit
// opens after the threshold → fallback served → service recovers → circuit closes → normal
// response. It also checks AC5 (state in Prometheus) and AC8 (settings reloaded live).
func TestCircuitBreakerAgainstAStoppedService(t *testing.T) {
	serverAddr, _ := startServer(t)
	register(t, serverAddr, "vault-111", "Vault-111")
	ai, corp := startRestartable(t, stub.KindAI), startRestartable(t, stub.KindCorporate)

	settings := filepath.Join(t.TempDir(), "breaker.yaml")
	writeSettings := func(content string) {
		if err := os.WriteFile(settings, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	writeSettings("defaults:\n  failure_threshold: 3\n  reset_timeout: 300ms\n  call_timeout: 500ms\n")
	gw, internal := breakerGateway(t, serverAddr, ai.addr, corp.addr, settings)

	status, body, _ := getEnvelope(t, gw+"/v1/nodes/vault-111/insights")
	if status != http.StatusOK || body["meta"].(map[string]any)["stale"] != nil {
		t.Fatalf("healthy service: %d %v", status, body)
	}
	if status, _, _ := getEnvelope(t, gw+"/v1/nodes/vault-111/corporate"); status != http.StatusOK {
		t.Fatalf("corporate: %d", status)
	}

	ai.stop() // the AI service goes down
	for i := 0; i < 3; i++ {
		_, _, _ = getEnvelope(t, gw+"/v1/nodes/vault-111/insights")
	}
	waitFor(t, "AI circuit open in Prometheus", func() bool {
		return strings.Contains(metricsText(t, internal), `eop_gateway_circuit_state{service="ai",track="stable",version="dev"} 2`)
	})
	if !strings.Contains(metricsText(t, internal), `eop_gateway_circuit_state{service="corporate",track="stable",version="dev"} 0`) {
		t.Fatal("the corporate circuit must stay closed: one breaker per service")
	}

	status, body, _ = getEnvelope(t, gw+"/v1/nodes/vault-111/insights")
	if status != http.StatusOK || body["meta"].(map[string]any)["stale"] != true {
		t.Fatalf("open circuit with cached data: %d %v", status, body)
	}
	status, body, header := getEnvelope(t, gw+"/v1/nodes/other-node/insights")
	if status != http.StatusServiceUnavailable || body["error"].(map[string]any)["code"] != "SERVICE_DEGRADED" || header.Get("Retry-After") == "" {
		t.Fatalf("open circuit without cached data: %d %v", status, body)
	}

	ai.start(ai.addr) // the AI service comes back on the same address
	waitFor(t, "fresh answer after recovery", func() bool {
		status, body, _ := getEnvelope(t, gw+"/v1/nodes/vault-111/insights")
		return status == http.StatusOK && body["meta"].(map[string]any)["stale"] == nil
	})
	text := metricsText(t, internal)
	for _, want := range []string{
		`eop_gateway_circuit_state{service="ai",track="stable",version="dev"} 0`,
		`eop_circuit_breaker_state_transitions_total{from_state="closed",service="ai",to_state="open"`,
		`eop_circuit_breaker_state_transitions_total{from_state="open",service="ai",to_state="half_open"`,
		`eop_circuit_breaker_state_transitions_total{from_state="half_open",service="ai",to_state="closed"`,
		`eop_gateway_fallback_responses_total{kind="stale",service="ai"`,
	} {
		if !strings.Contains(text, want) {
			t.Errorf("metrics missing %q", want)
		}
	}

	// US-303 AC8: a new threshold applies without restarting the gateway.
	writeSettings("defaults:\n  failure_threshold: 1\n  reset_timeout: 1h\n  call_timeout: 500ms\n")
	time.Sleep(200 * time.Millisecond) // a few reload intervals
	corp.stop()
	_, _, _ = getEnvelope(t, gw+"/v1/nodes/vault-111/corporate")
	waitFor(t, "corporate circuit open after a single failure", func() bool {
		return strings.Contains(metricsText(t, internal), `eop_gateway_circuit_state{service="corporate",track="stable",version="dev"} 2`)
	})
}

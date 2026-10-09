//go:build integration

// Package integration runs the gateway against a real eop-server (US-301 AC8).
//
//	EOP_SERVER_BIN=../../build/dev/eop_server \
//	EOP_HPC_ENGINE_BIN=../../build/dev/hpc-engine/hpc_engine \
//	go test -tags integration ./test/integration/
package integration

import (
	"bufio"
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/propagation"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"

	apispec "github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/api"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/api"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/config"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

const startupTimeout = 10 * time.Second

func freePort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = l.Close() }()
	return l.Addr().(*net.TCPAddr).Port
}

// startServer runs the real eop_server binary on free ports and stops it at the end of the test.
// extraEnv adds KEY=value settings for the server.
func startServer(t *testing.T, extraEnv ...string) (addr string, stop func()) {
	t.Helper()
	bin := os.Getenv("EOP_SERVER_BIN")
	if bin == "" {
		t.Fatal("EOP_SERVER_BIN must point to the eop_server binary")
	}
	port := freePort(t)
	cmd := exec.Command(bin)
	cmd.Env = append(os.Environ(),
		"EOP_PORT="+strconv.Itoa(port),
		"EOP_METRICS_PORT="+strconv.Itoa(freePort(t)),
		"EOP_TRACING_ENABLED=0",
	)
	cmd.Env = append(cmd.Env, extraEnv...)
	cmd.Stdout, cmd.Stderr = io.Discard, io.Discard
	if err := cmd.Start(); err != nil {
		t.Fatalf("starting eop_server: %v", err)
	}
	stopped := false
	stop = func() {
		if !stopped {
			stopped = true
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}
	}
	t.Cleanup(stop)

	addr = "127.0.0.1:" + strconv.Itoa(port)
	deadline := time.Now().Add(startupTimeout)
	for time.Now().Before(deadline) {
		if conn, err := net.DialTimeout("tcp", addr, 200*time.Millisecond); err == nil {
			_ = conn.Close()
			return addr, stop
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal("eop_server did not start")
	return "", nil
}

// register registers a node and keeps its session open, so the node stays ONLINE.
func register(t *testing.T, addr, nodeID, bunker string) {
	t.Helper()
	conn, err := net.DialTimeout("tcp", addr, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	reader := bufio.NewReader(conn)
	if _, err := reader.ReadString('\n'); err != nil { // WELCOME
		t.Fatal(err)
	}
	line := "REGISTER node_id=" + nodeID + " bunker_name=" + bunker + " ip_address=10.0.0.7 capacity=42\n"
	if _, err := conn.Write([]byte(line)); err != nil {
		t.Fatal(err)
	}
	ack, err := reader.ReadString('\n')
	if err != nil || !strings.HasPrefix(ack, "ACK") {
		t.Fatalf("REGISTER answered %q, %v", ack, err)
	}
}

func gateway(t *testing.T, serverAddr string) *httptest.Server {
	t.Helper()
	cfg, err := config.Load(func(key string) string {
		return map[string]string{"EOP_SERVER_ADDR": serverAddr}[key]
	})
	if err != nil {
		t.Fatal(err)
	}
	client, err := eopclient.New(cfg.Server)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = client.Close() })
	spec, err := apispec.JSON()
	if err != nil {
		t.Fatal(err)
	}
	handler := api.NewServer(api.Options{
		Store: client, Logger: telemetry.NewLogger(io.Discard, "api-gateway"),
		Metrics: telemetry.NewMetrics("test", "stable"), OpenAPIJSON: spec, HealthTimeout: time.Second,
	}).Routes()
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)
	return srv
}

// enableTracing makes the gateway send traceparent= on every text command, which checks
// against the real server that the key is ignored by servers that do not read it (ADR-026).
func enableTracing(t *testing.T) {
	t.Helper()
	provider := sdktrace.NewTracerProvider()
	previous, previousPropagator := otel.GetTracerProvider(), otel.GetTextMapPropagator()
	otel.SetTracerProvider(provider)
	otel.SetTextMapPropagator(propagation.TraceContext{})
	t.Cleanup(func() {
		otel.SetTracerProvider(previous)
		otel.SetTextMapPropagator(previousPropagator)
		_ = provider.Shutdown(context.Background())
	})
}

type envelope struct {
	Data  json.RawMessage `json:"data"`
	Error *struct {
		Code string `json:"code"`
	} `json:"error"`
}

func getJSON(t *testing.T, url string) (int, envelope) {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = resp.Body.Close() }()
	if ct := resp.Header.Get("Content-Type"); ct != "application/json" {
		t.Fatalf("Content-Type = %q", ct)
	}
	var body envelope
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, body
}

type node struct {
	NodeID     string `json:"node_id"`
	BunkerName string `json:"bunker_name"`
	Status     string `json:"status"`
	IPAddress  string `json:"ip_address"`
	Capacity   int    `json:"capacity"`
	LastSeenAt string `json:"last_seen_at"`
}

func TestNodeAPIAgainstRealServer(t *testing.T) {
	enableTracing(t)
	serverAddr, stopServer := startServer(t)
	gw := gateway(t, serverAddr)

	t.Run("health check", func(t *testing.T) {
		if status, _ := getJSON(t, gw.URL+"/v1/health"); status != http.StatusOK {
			t.Fatalf("status = %d", status)
		}
	})

	t.Run("list nodes, empty registry", func(t *testing.T) {
		status, body := getJSON(t, gw.URL+"/v1/nodes")
		if status != http.StatusOK || string(body.Data) != "[]" {
			t.Fatalf("got %d %s", status, body.Data)
		}
	})

	register(t, serverAddr, "vault-101", "Vault-101")

	t.Run("list nodes, populated registry", func(t *testing.T) {
		status, body := getJSON(t, gw.URL+"/v1/nodes")
		var nodes []node
		if err := json.Unmarshal(body.Data, &nodes); err != nil || status != http.StatusOK || len(nodes) != 1 {
			t.Fatalf("got %d %s (%v)", status, body.Data, err)
		}
		n := nodes[0]
		if n.NodeID != "vault-101" || n.BunkerName != "Vault-101" || n.Status != "ONLINE" || n.IPAddress != "10.0.0.7" || n.Capacity != 42 {
			t.Fatalf("node = %+v", n)
		}
		if _, err := time.Parse(time.RFC3339, n.LastSeenAt); err != nil {
			t.Fatalf("last_seen_at %q is not RFC 3339", n.LastSeenAt)
		}
	})

	t.Run("get existing node", func(t *testing.T) {
		status, body := getJSON(t, gw.URL+"/v1/nodes/vault-101")
		var n node
		if err := json.Unmarshal(body.Data, &n); err != nil || status != http.StatusOK || n.NodeID != "vault-101" {
			t.Fatalf("got %d %s", status, body.Data)
		}
	})

	t.Run("get non-existing node", func(t *testing.T) {
		status, body := getJSON(t, gw.URL+"/v1/nodes/ghost")
		if status != http.StatusNotFound || body.Error == nil || body.Error.Code != api.CodeNodeNotFound {
			t.Fatalf("got %d %+v", status, body.Error)
		}
	})

	t.Run("server down makes the gateway not ready", func(t *testing.T) {
		stopServer()
		status, body := getJSON(t, gw.URL+"/v1/health")
		if status != http.StatusServiceUnavailable || body.Error == nil || body.Error.Code != api.CodeUpstreamUnavailable {
			t.Fatalf("got %d %+v", status, body.Error)
		}
	})
}

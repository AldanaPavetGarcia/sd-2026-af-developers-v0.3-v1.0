package app

import (
	"bufio"
	"bytes"
	"context"
	"io"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/config"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

// heartbeatServer greets and acknowledges HEARTBEAT, enough for the readiness probe.
func heartbeatServer(t *testing.T) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer func() { _ = conn.Close() }()
				_, _ = conn.Write([]byte("WELCOME Server v1.0\n"))
				reader := bufio.NewReader(conn)
				for {
					if _, err := reader.ReadString('\n'); err != nil {
						return
					}
					_, _ = conn.Write([]byte("ACK HEARTBEAT\n"))
				}
			}()
		}
	}()
	return listener.Addr().String()
}

func testConfig(t *testing.T, serverAddr string) config.Config {
	t.Helper()
	cfg, err := config.Load(func(key string) string {
		return map[string]string{"EOP_SERVER_ADDR": serverAddr, "EOP_TRACING_ENABLED": "0", "EOP_GATEWAY_SHUTDOWN_TIMEOUT": "2s"}[key]
	})
	if err != nil {
		t.Fatal(err)
	}
	return cfg
}

func localListener(t *testing.T) net.Listener {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	return l
}

func get(t *testing.T, url string) (int, string) {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatalf("GET %s: %v", url, err)
	}
	defer func() { _ = resp.Body.Close() }()
	body, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(body)
}

func TestRunServesAndShutsDownCleanly(t *testing.T) {
	cfg := testConfig(t, heartbeatServer(t))
	apiL, internalL := localListener(t), localListener(t)
	var logs bytes.Buffer
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- Run(ctx, cfg, telemetry.NewLogger(&logs, "api-gateway"), Listeners{API: apiL, Metrics: internalL})
	}()

	apiURL, internalURL := "http://"+apiL.Addr().String(), "http://"+internalL.Addr().String()
	if status, body := get(t, apiURL+"/v1/health"); status != http.StatusOK {
		t.Fatalf("/v1/health = %d %s", status, body)
	}
	if status, _ := get(t, internalURL+"/livez"); status != http.StatusNoContent {
		t.Fatalf("/livez = %d", status)
	}
	if status, body := get(t, internalURL+"/metrics"); status != http.StatusOK || !strings.Contains(body, "eop_gateway_http_requests_total") {
		t.Fatalf("/metrics = %d, request counter missing", status)
	}
	if status, _ := get(t, apiURL+"/metrics"); status != http.StatusNotFound {
		t.Fatal("/metrics must not be exposed on the public listener")
	}

	cancel() // what SIGTERM does in main
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run returned %v on a requested shutdown, want nil (exit code 0)", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Run did not return after shutdown was requested")
	}
	if !strings.Contains(logs.String(), "shutdown requested") {
		t.Fatalf("shutdown not logged: %s", logs.String())
	}
}

func TestRunReportsStartupErrors(t *testing.T) {
	logger := telemetry.NewLogger(io.Discard, "api-gateway")

	cfg := testConfig(t, "127.0.0.1:1")
	cfg.Server.MaxConns = 0
	if err := Run(context.Background(), cfg, logger, Listeners{}); err == nil {
		t.Fatal("invalid client options accepted")
	}

	cfg = testConfig(t, "127.0.0.1:1")
	cfg.ListenAddr = "bad-address"
	if err := Run(context.Background(), cfg, logger, Listeners{}); err == nil {
		t.Fatal("unusable listen address accepted")
	}

	cfg = testConfig(t, "127.0.0.1:1")
	cfg.MetricsAddr = "bad-address"
	if err := Run(context.Background(), cfg, logger, Listeners{API: localListener(t)}); err == nil {
		t.Fatal("unusable metrics address accepted")
	}
}

func TestRunStopsWhenAServerFails(t *testing.T) {
	cfg := testConfig(t, heartbeatServer(t))
	apiL := localListener(t)
	internalL := localListener(t)
	_ = internalL.Close() // Serve fails immediately on a closed listener
	err := Run(context.Background(), cfg, telemetry.NewLogger(io.Discard, "api-gateway"), Listeners{API: apiL, Metrics: internalL})
	if err == nil {
		t.Fatal("Run returned nil although a server failed")
	}
}

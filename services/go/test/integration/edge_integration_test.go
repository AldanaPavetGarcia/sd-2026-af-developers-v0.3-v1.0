//go:build integration

package integration

import (
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"strconv"
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"
	mochi "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/hooks/auth"
	"github.com/mochi-mqtt/server/v2/listeners"
	"github.com/redis/go-redis/v9"

	apispec "github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/api"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/api"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/config"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/ingest"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/sim"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

// edgeRig is the edge path of US-404 AC8 on one machine: an in-process MQTT broker, Redis
// (miniredis), the edge ingestor, the simulator, the real eop_server and the gateway.
type edgeRig struct {
	brokerURL  string
	redis      *redis.Client
	gatewayURL string
}

func startBroker(t *testing.T) string {
	t.Helper()
	addr := "127.0.0.1:" + strconv.Itoa(freePort(t))
	server := mochi.New(&mochi.Options{InlineClient: true})
	if err := server.AddHook(new(auth.AllowHook), nil); err != nil {
		t.Fatal(err)
	}
	if err := server.AddListener(listeners.NewTCP(listeners.Config{ID: "tcp", Address: addr})); err != nil {
		t.Fatal(err)
	}
	go func() { _ = server.Serve() }()
	t.Cleanup(func() { _ = server.Close() })
	return "tcp://" + addr
}

// startServerOn runs eop_server on a given port (the ingestor may know the address before the
// server exists).
func startServerOn(t *testing.T, port int) {
	t.Helper()
	cmd := exec.Command(os.Getenv("EOP_SERVER_BIN"))
	cmd.Env = append(os.Environ(), "EOP_PORT="+strconv.Itoa(port), "EOP_METRICS_PORT="+strconv.Itoa(freePort(t)), "EOP_TRACING_ENABLED=0")
	cmd.Stdout, cmd.Stderr = io.Discard, io.Discard
	if err := cmd.Start(); err != nil {
		t.Fatalf("starting eop_server: %v", err)
	}
	t.Cleanup(func() { _ = cmd.Process.Kill(); _ = cmd.Wait() })
	deadline := time.Now().Add(startupTimeout)
	for time.Now().Before(deadline) {
		if conn, err := net.DialTimeout("tcp", "127.0.0.1:"+strconv.Itoa(port), 200*time.Millisecond); err == nil {
			_ = conn.Close()
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal("eop_server did not start")
}

func newEdgeRig(t *testing.T, serverAddr string) *edgeRig {
	t.Helper()
	if os.Getenv("EOP_SERVER_BIN") == "" {
		t.Fatal("EOP_SERVER_BIN must point to the eop_server binary")
	}
	mr := miniredis.RunT(t)
	rig := &edgeRig{brokerURL: startBroker(t), redis: redis.NewClient(&redis.Options{Addr: mr.Addr()})}

	cfg, err := ingest.Load(func(key string) string {
		return map[string]string{
			"EOP_MQTT_BROKER": rig.brokerURL, "EOP_SERVER_ADDR": serverAddr, "EOP_REDIS_ADDR": mr.Addr(),
			"EOP_TRACING_ENABLED": "0", "EOP_EDGE_STATUS_INTERVAL": "100ms", "EOP_EDGE_STATUS_TTL": "2s",
			"EOP_EDGE_RETRY_INITIAL": "50ms", "EOP_EDGE_RETRY_MAX": "200ms",
		}[key]
	})
	if err != nil {
		t.Fatal(err)
	}
	metrics, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		_ = ingest.Run(ctx, cfg, telemetry.NewLogger(io.Discard, "edge-ingestor"), metrics)
		close(done)
	}()
	t.Cleanup(func() { cancel(); <-done })

	gcfg, err := config.Load(func(key string) string { return map[string]string{"EOP_SERVER_ADDR": serverAddr}[key] })
	if err != nil {
		t.Fatal(err)
	}
	client, err := eopclient.New(gcfg.Server)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = client.Close() })
	spec, err := apispec.JSON()
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(api.NewServer(api.Options{
		Store: client, Edge: edge.RedisStatus{RDB: rig.redis}, Logger: telemetry.NewLogger(io.Discard, "api-gateway"),
		Metrics: telemetry.NewMetrics("test", "stable"), OpenAPIJSON: spec, HealthTimeout: time.Second,
	}).Routes())
	t.Cleanup(srv.Close)
	rig.gatewayURL = srv.URL

	// The ingestor is subscribed once it reports a connected state.
	waitUntil(t, "the ingestor to connect", func() bool {
		s, ok, _ := edge.ReadSnapshot(context.Background(), rig.redis)
		return ok && s.State == edge.StateConnected
	})
	return rig
}

func (r *edgeRig) publish(t *testing.T, devices, count int) {
	t.Helper()
	cfg := sim.Config{Broker: r.brokerURL, TopicPrefix: "eop/bunker", NodePrefix: "sim-vault-", Devices: devices,
		Period: 20 * time.Millisecond, Count: count, Timeout: 5 * time.Second}
	sent, err := sim.Run(context.Background(), cfg, sim.MQTTDial(cfg), time.Now, telemetry.NewLogger(io.Discard, "edge-sim"))
	if err != nil || sent != devices*count {
		t.Fatalf("simulator sent %d, %v", sent, err)
	}
}

func (r *edgeRig) get(t *testing.T, path string, out any) {
	t.Helper()
	resp, err := http.Get(r.gatewayURL + path)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET %s -> %d", path, resp.StatusCode)
	}
	if err := json.NewDecoder(resp.Body).Decode(out); err != nil {
		t.Fatal(err)
	}
}

func waitUntil(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

type edgeStatusBody struct {
	Data edge.Snapshot `json:"data"`
}

type nodesBody struct {
	Data []struct {
		NodeID    string `json:"node_id"`
		Status    string `json:"status"`
		Source    string `json:"source"`
		Telemetry *struct {
			Temperature float64 `json:"temperature"`
			Status      string  `json:"status"`
			SampledAt   string  `json:"sampled_at"`
		} `json:"telemetry"`
	} `json:"data"`
}

// US-404 AC8: the simulator publishes 10 messages, the ingestor receives and forwards all 10,
// eop-server registers them, and GET /v1/nodes shows the device with its telemetry.
func TestEdgeTenMessagesReachNodes(t *testing.T) {
	addr, _ := startServer(t)
	rig := newEdgeRig(t, addr)
	started := time.Now()
	rig.publish(t, 1, 10)

	var status edgeStatusBody
	waitUntil(t, "10 forwarded messages", func() bool {
		rig.get(t, "/v1/edge/status", &status)
		return status.Data.Forwarded >= 10
	})
	if status.Data.State != edge.StateConnected || status.Data.Received != 10 || status.Data.Forwarded != 10 ||
		status.Data.Duplicates != 0 || status.Data.Invalid != 0 {
		t.Fatalf("edge status %+v", status.Data)
	}

	var nodes nodesBody
	rig.get(t, "/v1/nodes", &nodes)
	for _, n := range nodes.Data {
		if n.NodeID != "sim-vault-1" {
			continue
		}
		if n.Source != "edge" || n.Status != "ONLINE" || n.Telemetry == nil {
			t.Fatalf("edge node %+v", n)
		}
		sampled, err := time.Parse(time.RFC3339Nano, n.Telemetry.SampledAt)
		if err != nil || sampled.Before(started.Add(-time.Second)) {
			t.Fatalf("sampled_at %q", n.Telemetry.SampledAt)
		}
		return
	}
	t.Fatalf("sim-vault-1 missing from /v1/nodes: %+v", nodes.Data)
}

// US-404 AC5 with the real server: published while eop-server is down, delivered when it starts.
func TestEdgeServerDownLosesNothing(t *testing.T) {
	port := freePort(t)
	rig := newEdgeRig(t, "127.0.0.1:"+strconv.Itoa(port))
	rig.publish(t, 2, 5)
	time.Sleep(300 * time.Millisecond) // the ingestor is retrying the first message

	var status edgeStatusBody
	startServerOn(t, port)
	waitUntil(t, "every message after the server started", func() bool {
		rig.get(t, "/v1/edge/status", &status)
		return status.Data.Forwarded >= 10
	})
	if status.Data.Received != 10 || status.Data.Forwarded != 10 {
		t.Fatalf("edge status %+v", status.Data)
	}
}

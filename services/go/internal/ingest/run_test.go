package ingest

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"
	mochi "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/hooks/auth"
	"github.com/mochi-mqtt/server/v2/listeners"
	"github.com/redis/go-redis/v9"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
)

func freeAddr(t *testing.T) string {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := l.Addr().String()
	_ = l.Close()
	return addr
}

// startBroker runs an in-process MQTT broker; the test publishes through its inline client.
func startBroker(t *testing.T) (*mochi.Server, string) {
	t.Helper()
	addr := freeAddr(t)
	server := mochi.New(&mochi.Options{InlineClient: true})
	if err := server.AddHook(new(auth.AllowHook), nil); err != nil {
		t.Fatal(err)
	}
	if err := server.AddListener(listeners.NewTCP(listeners.Config{ID: "tcp", Address: addr})); err != nil {
		t.Fatal(err)
	}
	go func() { _ = server.Serve() }()
	t.Cleanup(func() { _ = server.Close() })
	return server, "tcp://" + addr
}

// fakeServer plays eop-server for TELEMETRY: ACK while up, closed connections while down.
type fakeServer struct {
	listener net.Listener
	up       atomic.Bool
	mu       sync.Mutex
	lines    []string
}

func startFakeServer(t *testing.T) *fakeServer {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s := &fakeServer{listener: l}
	s.up.Store(true)
	go func() {
		for {
			conn, err := l.Accept()
			if err != nil {
				return
			}
			go s.serve(conn)
		}
	}()
	t.Cleanup(func() { _ = l.Close() })
	return s
}

func (s *fakeServer) serve(conn net.Conn) {
	defer func() { _ = conn.Close() }()
	if !s.up.Load() {
		return
	}
	_, _ = io.WriteString(conn, "WELCOME Server v1.0 | TIME 0 | SESSION_ID 1\n")
	reader := bufio.NewReader(conn)
	for {
		line, err := reader.ReadString('\n')
		if err != nil || !s.up.Load() {
			return
		}
		line = strings.TrimSpace(line)
		s.mu.Lock()
		s.lines = append(s.lines, line)
		s.mu.Unlock()
		var node, msg string
		for _, field := range strings.Fields(line) {
			if v, ok := strings.CutPrefix(field, "node_id="); ok {
				node = v
			}
			if v, ok := strings.CutPrefix(field, "message_id="); ok {
				msg = v
			}
		}
		_, _ = io.WriteString(conn, "ACK TELEMETRY node_id="+node+" message_id="+msg+"\n")
	}
}

func (s *fakeServer) received() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.lines...)
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

type ingestor struct {
	cancel  context.CancelFunc
	done    chan error
	metrics string
}

func startIngestor(t *testing.T, broker, server, redisAddr string) *ingestor {
	t.Helper()
	cfg, err := Load(func(key string) string {
		return map[string]string{
			"EOP_MQTT_BROKER": broker, "EOP_SERVER_ADDR": server, "EOP_REDIS_ADDR": redisAddr,
			"EOP_TRACING_ENABLED": "false", "EOP_EDGE_RETRY_INITIAL": "20ms", "EOP_EDGE_RETRY_MAX": "50ms",
			"EOP_EDGE_STATUS_INTERVAL": "50ms", "EOP_EDGE_STATUS_TTL": "1s", "EOP_MQTT_MAX_RECONNECT_INTERVAL": "200ms",
		}[key]
	})
	if err != nil {
		t.Fatal(err)
	}
	metricsListener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	in := &ingestor{cancel: cancel, done: make(chan error, 1), metrics: "http://" + metricsListener.Addr().String() + "/metrics"}
	go func() { in.done <- Run(ctx, cfg, slog.New(slog.NewTextHandler(io.Discard, nil)), metricsListener) }()
	return in
}

func (in *ingestor) stop(t *testing.T) {
	t.Helper()
	in.cancel()
	select {
	case err := <-in.done:
		if err != nil {
			t.Fatalf("Run returned %v", err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("Run did not stop")
	}
}

func publish(t *testing.T, broker *mochi.Server, nodeID string, messageID int) {
	t.Helper()
	if err := broker.Publish(edge.Topic(prefix, nodeID, edge.KindTelemetry), telemetryPayload(nodeID, messageID), false, 1); err != nil {
		t.Fatal(err)
	}
}

// US-404 AC1, AC2, AC3, AC7 through a real MQTT session.
func TestIngestorForwardsTenMessagesOnceEach(t *testing.T) {
	broker, brokerURL := startBroker(t)
	server := startFakeServer(t)
	mr := miniredis.RunT(t)
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr()})
	in := startIngestor(t, brokerURL, server.listener.Addr().String(), mr.Addr())
	defer in.stop(t)

	waitFor(t, "a connected snapshot", func() bool {
		s, ok, _ := edge.ReadSnapshot(context.Background(), rdb)
		return ok && s.State == edge.StateConnected
	})
	for i := 1; i <= 10; i++ {
		publish(t, broker, "vault-101", i)
	}
	publish(t, broker, "vault-101", 10) // redelivered by the device after a lost PUBACK
	waitFor(t, "10 TELEMETRY commands", func() bool { return len(server.received()) >= 10 })
	waitFor(t, "the snapshot to count the duplicate", func() bool {
		s, _, _ := edge.ReadSnapshot(context.Background(), rdb)
		return s.Duplicates == 1
	})
	time.Sleep(100 * time.Millisecond)
	lines := server.received()
	if len(lines) != 10 {
		t.Fatalf("server received %d commands, want 10 (duplicate not forwarded)", len(lines))
	}
	for i, line := range lines {
		if !strings.HasPrefix(line, fmt.Sprintf("TELEMETRY node_id=vault-101 bunker_name=Vault-101 message_id=%d boot_id=9f3c27a1 temperature=21.4 status=OK sampled_at=", i+1)) {
			t.Fatalf("command %d = %q", i, line)
		}
	}
	snap, _, _ := edge.ReadSnapshot(context.Background(), rdb)
	if snap.Received != 11 || snap.Forwarded != 10 || len(snap.Devices) != 1 || snap.Devices[0].NodeID != "vault-101" {
		t.Fatalf("snapshot %+v", snap)
	}
	resp, err := http.Get(in.metrics)
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	for _, want := range []string{"eop_edge_messages_forwarded_total 10", "eop_edge_messages_duplicate_total 1", "eop_edge_broker_connected 1", "eop_edge_e2e_latency_seconds_bucket"} {
		if !strings.Contains(string(body), want) {
			t.Errorf("metrics lack %q", want)
		}
	}
}

// US-404 AC5 and ADR-028 §2: while eop-server is down the message is retried and not
// acknowledged; an ingestor stopped meanwhile gets it again from the broker after restarting.
func TestUnacknowledgedMessageSurvivesAnIngestorRestart(t *testing.T) {
	broker, brokerURL := startBroker(t)
	server := startFakeServer(t)
	server.up.Store(false)
	mr := miniredis.RunT(t)
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr()})

	first := startIngestor(t, brokerURL, server.listener.Addr().String(), mr.Addr())
	waitFor(t, "a connected snapshot", func() bool {
		s, ok, _ := edge.ReadSnapshot(context.Background(), rdb)
		return ok && s.State == edge.StateConnected
	})
	publish(t, broker, "vault-101", 1)
	waitFor(t, "a retry", func() bool {
		resp, err := http.Get(first.metrics)
		if err != nil {
			return false
		}
		defer func() { _ = resp.Body.Close() }()
		body, _ := io.ReadAll(resp.Body)
		return strings.Contains(string(body), "eop_edge_forward_retries_total") &&
			!strings.Contains(string(body), "eop_edge_forward_retries_total 0")
	})
	first.stop(t)
	if len(server.received()) != 0 {
		t.Fatal("nothing can have been delivered while the server was down")
	}

	server.up.Store(true)
	second := startIngestor(t, brokerURL, server.listener.Addr().String(), mr.Addr())
	defer second.stop(t)
	waitFor(t, "the redelivered message", func() bool { return len(server.received()) == 1 })
	if !strings.Contains(server.received()[0], "message_id=1 ") {
		t.Fatalf("got %q", server.received()[0])
	}
}

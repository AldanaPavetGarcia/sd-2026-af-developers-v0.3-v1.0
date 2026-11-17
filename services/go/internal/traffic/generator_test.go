package traffic

import (
	"bufio"
	"bytes"
	"context"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/testutil"
)

// fakeServer answers like eop-server: WELCOME, then ACK to REGISTER and HEARTBEAT. With
// dropAfter > 0 it closes each session after that many heartbeats.
type fakeServer struct {
	ln        net.Listener
	dropAfter int
	duplicate atomic.Bool
	mu        sync.Mutex
	registers []string
}

func startServer(t *testing.T, dropAfter int) *fakeServer {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s := &fakeServer{ln: ln, dropAfter: dropAfter}
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go s.serve(conn)
		}
	}()
	t.Cleanup(func() { _ = ln.Close() })
	return s
}

func (s *fakeServer) serve(conn net.Conn) {
	defer func() { _ = conn.Close() }()
	_, _ = conn.Write([]byte("WELCOME Server v1.0 | TIME 0 | SESSION_ID s\n"))
	reader := bufio.NewReader(conn)
	beats := 0
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return
		}
		switch {
		case strings.HasPrefix(line, "REGISTER "):
			s.mu.Lock()
			s.registers = append(s.registers, strings.TrimSpace(line))
			s.mu.Unlock()
			if s.duplicate.Load() {
				_, _ = conn.Write([]byte("ERROR_DUPLICATE node_id=x desc=Node already registered\n"))
				continue
			}
			_, _ = conn.Write([]byte("ACK Server v1.0 | TIME 0 | SESSION_ID x\n"))
		case strings.HasPrefix(line, "HEARTBEAT"):
			beats++
			if s.dropAfter > 0 && beats > s.dropAfter {
				return
			}
			_, _ = conn.Write([]byte("ACK HEARTBEAT\n"))
		}
	}
}

func (s *fakeServer) registered() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.registers...)
}

type fakeGateway struct {
	mu       sync.Mutex
	paths    []string
	analyzed int
	failNode bool
}

func (g *fakeGateway) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	g.mu.Lock()
	defer g.mu.Unlock()
	if r.Method == http.MethodPost {
		body, _ := io.ReadAll(r.Body)
		if r.URL.Path == "/v1/graph/analyze" && bytes.Equal(body, analyzeBody) {
			g.analyzed++
			_, _ = w.Write([]byte(`{"data":{}}`))
			return
		}
		http.Error(w, "bad", http.StatusBadRequest)
		return
	}
	g.paths = append(g.paths, r.URL.Path)
	if g.failNode && strings.HasPrefix(r.URL.Path, "/v1/nodes/") {
		http.Error(w, "down", http.StatusServiceUnavailable)
		return
	}
	_, _ = w.Write([]byte(`{"data":{}}`))
}

func newGenerator(t *testing.T, cfg Config) (*Generator, *bytes.Buffer) {
	t.Helper()
	logs := &bytes.Buffer{}
	return &Generator{
		Config:  cfg,
		Metrics: NewMetrics(prometheus.NewRegistry()),
		Logger:  slog.New(slog.NewJSONHandler(&lockedWriter{w: logs}, nil)),
		HTTP:    &http.Client{Timeout: time.Second},
	}, logs
}

type lockedWriter struct {
	mu sync.Mutex
	w  *bytes.Buffer
}

func (l *lockedWriter) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.w.Write(p)
}

func baseConfig(gatewayURL, serverAddr string) Config {
	cfg := defaults()
	cfg.GatewayURL = gatewayURL
	cfg.ServerAddr = serverAddr
	cfg.Nodes = 2
	cfg.GatewayInterval = 10 * time.Millisecond
	cfg.AnalyzeInterval = 50 * time.Millisecond
	cfg.HeartbeatInterval = 20 * time.Millisecond
	cfg.RequestTimeout = time.Second
	cfg.SummaryEvery = 100 * time.Millisecond
	return cfg
}

func count(m *Metrics, target, op, result string) float64 {
	return testutil.ToFloat64(m.requests.WithLabelValues(target, op, result))
}

func TestGeneratorSendsEveryKindOfRequest(t *testing.T) {
	server := startServer(t, 0)
	gw := &fakeGateway{}
	httpSrv := httptest.NewServer(gw)
	defer httpSrv.Close()
	gen, logs := newGenerator(t, baseConfig(httpSrv.URL, server.ln.Addr().String()))

	ctx, cancel := context.WithTimeout(context.Background(), 400*time.Millisecond)
	defer cancel()
	if err := gen.Run(ctx); err != context.DeadlineExceeded {
		t.Fatalf("Run: %v", err)
	}

	m := gen.Metrics
	if count(m, targetServer, opRegister, resultOK) != 2 {
		t.Fatalf("registers: %v", server.registered())
	}
	want := map[string]bool{
		"REGISTER node_id=slo-bunker-1 bunker_name=SLO-1 ip_address=10.99.0.1 capacity=1": true,
		"REGISTER node_id=slo-bunker-2 bunker_name=SLO-2 ip_address=10.99.0.2 capacity=1": true,
	}
	for _, r := range server.registered() {
		if !want[r] {
			t.Fatalf("unexpected %q", r)
		}
	}
	if count(m, targetServer, opHeartbeat, resultOK) < 10 {
		t.Fatal("too few heartbeats")
	}
	for _, route := range []string{"/v1/health", "/v1/nodes", "/v1/nodes/{node}"} {
		if count(m, targetGateway, route, resultOK) < 5 {
			t.Fatalf("too few %s", route)
		}
	}
	if count(m, targetGateway, opAnalyze, resultOK) < 5 {
		t.Fatal("too few analyses")
	}
	gw.mu.Lock()
	for _, p := range gw.paths {
		if strings.Contains(p, "{") {
			t.Fatalf("template not filled in: %s", p)
		}
	}
	gw.mu.Unlock()
	if testutil.ToFloat64(m.online) != 0 {
		t.Fatal("nodes still counted as registered after Run")
	}
	if !strings.Contains(logs.String(), "synthetic traffic summary") {
		t.Fatal("no summary logged")
	}
}

// Failures are counted and logged, and a node that loses its session registers again.
func TestGeneratorRecoversFromFailures(t *testing.T) {
	server := startServer(t, 2)
	gw := &fakeGateway{failNode: true}
	httpSrv := httptest.NewServer(gw)
	defer httpSrv.Close()
	cfg := baseConfig(httpSrv.URL, server.ln.Addr().String())
	cfg.Nodes = 1
	cfg.AnalyzeInterval = 0
	gen, logs := newGenerator(t, cfg)

	ctx, cancel := context.WithTimeout(context.Background(), 1500*time.Millisecond)
	defer cancel()
	_ = gen.Run(ctx)

	m := gen.Metrics
	if count(m, targetGateway, "/v1/nodes/{node}", resultError) == 0 {
		t.Fatal("HTTP 503 not counted as an error")
	}
	if count(m, targetServer, opHeartbeat, resultError) == 0 {
		t.Fatal("dropped session not counted")
	}
	if count(m, targetServer, opRegister, resultOK) < 2 {
		t.Fatalf("the node did not register again: %v", server.registered())
	}
	if count(m, targetGateway, opAnalyze, resultOK)+count(m, targetGateway, opAnalyze, resultError) != 0 {
		t.Fatal("analysis disabled but sent")
	}
	if !strings.Contains(logs.String(), "registering again") {
		t.Fatal("lost session not logged")
	}
}

func TestRegisterRejectedAndServerDown(t *testing.T) {
	server := startServer(t, 0)
	server.duplicate.Store(true)
	gen, _ := newGenerator(t, baseConfig("http://127.0.0.1:1", server.ln.Addr().String()))
	if err := gen.nodeSession(context.Background(), "slo-bunker-1", 1); err == nil || !strings.Contains(err.Error(), "ERROR_DUPLICATE") {
		t.Fatalf("got %v", err)
	}

	gen.Config.ServerAddr = "127.0.0.1:1"
	if err := gen.nodeSession(context.Background(), "slo-bunker-1", 1); err == nil {
		t.Fatal("unreachable server accepted")
	}
	if count(gen.Metrics, targetServer, opRegister, resultError) != 2 {
		t.Fatal("register errors not counted")
	}
	// A gateway that is down, and a request that cannot be built.
	if err := gen.get(context.Background(), "/v1/health"); err == nil {
		t.Fatal("unreachable gateway accepted")
	}
	gen.Config.GatewayURL = "http://bad host"
	if gen.get(context.Background(), "/x") == nil || gen.post(context.Background(), "/x", nil) == nil {
		t.Fatal("bad URL accepted")
	}
}

func TestErrString(t *testing.T) {
	if errString(nil) != "" || errString(io.EOF) != "EOF" {
		t.Fatal("errString")
	}
}

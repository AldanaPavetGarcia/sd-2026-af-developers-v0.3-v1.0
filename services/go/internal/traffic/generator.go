package traffic

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// maxRetryDelay bounds the wait before a node registers again after losing its session.
const maxRetryDelay = 30 * time.Second

// analyzeBody is a 3-node graph: small enough to be analysed synchronously by the gateway.
var analyzeBody = []byte(`{"nodes":["a","b","c"],"edges":[{"from":"a","to":"b","weight":1},` +
	`{"from":"b","to":"c","weight":2}],"algorithms":["shortest_paths","connected_components"]}`)

// Generator runs the load described by its Config.
type Generator struct {
	Config  Config
	Metrics *Metrics
	Logger  *slog.Logger
	HTTP    *http.Client

	sent   atomic.Int64
	failed atomic.Int64
}

// Run sends traffic until ctx ends.
func (g *Generator) Run(ctx context.Context) error {
	var wg sync.WaitGroup
	start := func(f func(context.Context)) {
		wg.Add(1)
		go func() {
			defer wg.Done()
			f(ctx)
		}()
	}
	start(g.gatewayLoop)
	if g.Config.AnalyzeInterval > 0 {
		start(g.analyzeLoop)
	}
	for n := 1; n <= g.Config.Nodes; n++ {
		id := g.Config.NodeID(n)
		start(func(ctx context.Context) { g.nodeLoop(ctx, id, n) })
	}
	start(g.summaryLoop)
	wg.Wait()
	return ctx.Err()
}

func (g *Generator) record(target, op string, started time.Time, err error) {
	g.Metrics.observe(target, op, time.Since(started).Seconds(), err)
	g.sent.Add(1)
	if err != nil {
		g.failed.Add(1)
		g.Logger.Warn("synthetic request failed", "target", target, "op", op, "error", err.Error())
	}
}

// every calls f now and then once per interval, until ctx ends or stop (if not nil) says so.
func every(ctx context.Context, interval time.Duration, f func(), stop func() bool) {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		f()
		if stop != nil && stop() {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (g *Generator) gatewayLoop(ctx context.Context) {
	i := 0
	every(ctx, g.Config.GatewayInterval, func() {
		// The template is the metric label (bounded); the request gets a real node_id.
		route := g.Config.GatewayPaths[i%len(g.Config.GatewayPaths)]
		path := route
		if g.Config.Nodes > 0 {
			path = strings.ReplaceAll(route, "{node}", g.Config.NodeID(i%g.Config.Nodes+1))
		}
		i++
		started := time.Now()
		err := g.get(ctx, path)
		if ctx.Err() == nil {
			g.record(targetGateway, route, started, err)
		}
	}, nil)
}

func (g *Generator) analyzeLoop(ctx context.Context) {
	every(ctx, g.Config.AnalyzeInterval, func() {
		started := time.Now()
		err := g.post(ctx, "/v1/graph/analyze", analyzeBody)
		if ctx.Err() == nil {
			g.record(targetGateway, opAnalyze, started, err)
		}
	}, nil)
}

// A 4xx or 5xx is a failed request: the load only sends valid ones.
func (g *Generator) do(req *http.Request) error {
	resp, err := g.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer func() { _ = resp.Body.Close() }()
	_, _ = io.Copy(io.Discard, resp.Body)
	if resp.StatusCode >= http.StatusBadRequest {
		return fmt.Errorf("%s %s: HTTP %d", req.Method, req.URL.Path, resp.StatusCode)
	}
	return nil
}

func (g *Generator) get(ctx context.Context, path string) error {
	ctx, cancel := context.WithTimeout(ctx, g.Config.RequestTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, g.Config.GatewayURL+path, nil)
	if err != nil {
		return err
	}
	return g.do(req)
}

func (g *Generator) post(ctx context.Context, path string, body []byte) error {
	ctx, cancel := context.WithTimeout(ctx, g.Config.RequestTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, g.Config.GatewayURL+path, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	return g.do(req)
}

// nodeLoop keeps one bunker registered: REGISTER, then HEARTBEAT on the same session; after any
// failure it registers again with exponential backoff.
func (g *Generator) nodeLoop(ctx context.Context, nodeID string, n int) {
	delay := time.Second
	for ctx.Err() == nil {
		err := g.nodeSession(ctx, nodeID, n)
		if ctx.Err() != nil {
			return
		}
		g.Logger.Warn("synthetic node lost its session; registering again", "node_id", nodeID,
			"error", errString(err), "retry_in", delay.String())
		select {
		case <-ctx.Done():
			return
		case <-time.After(delay):
		}
		delay = min(2*delay, maxRetryDelay)
	}
}

func errString(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

type session struct {
	conn   net.Conn
	reader *bufio.Reader
	g      *Generator
}

// exchange sends one command and returns the first answer line that is not the WELCOME banner.
func (s *session) exchange(command string) (string, error) {
	if err := s.conn.SetDeadline(time.Now().Add(s.g.Config.RequestTimeout)); err != nil {
		return "", err
	}
	if _, err := s.conn.Write([]byte(command + "\n")); err != nil {
		return "", err
	}
	for {
		line, err := s.reader.ReadString('\n')
		if err != nil {
			return "", err
		}
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "WELCOME") {
			return line, nil
		}
	}
}

func (g *Generator) nodeSession(ctx context.Context, nodeID string, n int) error {
	var dialer net.Dialer
	dialCtx, cancel := context.WithTimeout(ctx, g.Config.RequestTimeout)
	conn, err := dialer.DialContext(dialCtx, "tcp", g.Config.ServerAddr)
	cancel()
	if err != nil {
		g.record(targetServer, opRegister, time.Now(), err)
		return err
	}
	defer func() { _ = conn.Close() }()
	s := &session{conn: conn, reader: bufio.NewReader(conn), g: g}

	started := time.Now()
	answer, err := s.exchange(fmt.Sprintf("REGISTER node_id=%s bunker_name=SLO-%d ip_address=10.99.0.%d capacity=1",
		nodeID, n, n))
	if err == nil && !strings.HasPrefix(answer, "ACK") {
		err = errors.New("REGISTER answered " + answer)
	}
	g.record(targetServer, opRegister, started, err)
	if err != nil {
		return err
	}
	g.Metrics.online.Inc()
	defer g.Metrics.online.Dec()

	var sessionErr error
	every(ctx, g.Config.HeartbeatInterval, func() {
		if sessionErr != nil || ctx.Err() != nil {
			return
		}
		started := time.Now()
		answer, err := s.exchange("HEARTBEAT")
		if err == nil && !strings.HasPrefix(answer, "ACK") {
			err = errors.New("HEARTBEAT answered " + answer)
		}
		if ctx.Err() != nil {
			return
		}
		g.record(targetServer, opHeartbeat, started, err)
		if err != nil {
			sessionErr = err
		}
	}, func() bool { return sessionErr != nil })
	return sessionErr
}

func (g *Generator) summaryLoop(ctx context.Context) {
	ticker := time.NewTicker(g.Config.SummaryEvery)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			g.Logger.Info("synthetic traffic summary", "requests", g.sent.Swap(0), "failed", g.failed.Swap(0),
				"period", g.Config.SummaryEvery.String())
		}
	}
}

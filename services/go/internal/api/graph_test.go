package api

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"
	"github.com/redis/go-redis/v9"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/analysis"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/jobs"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

const asyncMinNodes = 5 // small, so tests can build "large" graphs cheaply

var engineNode = eopclient.Node{NodeID: "hpc-engine", Status: "ONLINE", LastSeen: time.Unix(0, 0)}

type fakeAnalyzer struct {
	mu       sync.Mutex
	requests []eopclient.AnalyzeRequest
	answer   func(ctx context.Context) (json.RawMessage, error)
}

func (f *fakeAnalyzer) AnalyzeGraph(ctx context.Context, _ string, req eopclient.AnalyzeRequest) (json.RawMessage, error) {
	f.mu.Lock()
	f.requests = append(f.requests, req)
	f.mu.Unlock()
	return f.answer(ctx)
}

func (f *fakeAnalyzer) calls() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.requests)
}

func okResult(context.Context) (json.RawMessage, error) {
	return json.RawMessage(`{"graph_size":{"nodes":3,"edges":2},"algorithms":{"connected_components":[["A","B","C"]]}}`), nil
}

type graphHarness struct {
	*harness
	analyzer *fakeAnalyzer
	store    *fakeStore
	jobStore *jobs.Store
	runner   *jobs.Runner
	redis    *miniredis.Miniredis
}

func newGraphHarness(t *testing.T, answer func(context.Context) (json.RawMessage, error)) *graphHarness {
	t.Helper()
	mr := miniredis.RunT(t)
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr(), MaxRetries: -1, DialTimeout: 200 * time.Millisecond})
	t.Cleanup(func() { _ = rdb.Close() })
	g := &graphHarness{
		analyzer: &fakeAnalyzer{answer: answer},
		store:    &fakeStore{nodes: []eopclient.Node{engineNode}},
		jobStore: jobs.NewStore(rdb, 5*time.Minute, 30*time.Minute),
		redis:    mr,
	}
	g.runner = jobs.NewRunner(g.jobStore, g.analyzer, telemetryDiscard(), 1, 2)
	t.Cleanup(func() { _ = g.runner.Stop(context.Background()) })
	g.harness = newHarness(t, g.store, func(o *Options) {
		o.Analyzer = g.analyzer
		o.JobRunner = g.runner
		o.JobStore = g.jobStore
		o.Analysis = AnalysisOptions{MaxBodyBytes: 4096, AsyncMinNodes: asyncMinNodes, SyncTimeout: time.Second}
	})
	return g
}

func graphBody(nodes int, algorithms ...string) string {
	if len(algorithms) == 0 {
		algorithms = []string{"connected_components"}
	}
	names := make([]string, nodes)
	edges := make([]string, 0, nodes)
	for i := range names {
		names[i] = fmt.Sprintf("%q", fmt.Sprintf("N%d", i))
		if i > 0 {
			edges = append(edges, fmt.Sprintf(`{"from":"N%d","to":"N%d","weight":1}`, i-1, i))
		}
	}
	algos, _ := json.Marshal(algorithms)
	return `{"nodes":[` + strings.Join(names, ",") + `],"edges":[` + strings.Join(edges, ",") + `],"algorithms":` + string(algos) + `}`
}

func TestAnalyzeSmallGraphSynchronously(t *testing.T) {
	g := newGraphHarness(t, okResult)
	rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(3))
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d %s", rec.Code, rec.Body.String())
	}
	result := decode(t, rec)["data"].(map[string]any)["result"].(map[string]any)
	if result["graph_size"].(map[string]any)["nodes"] != 3.0 {
		t.Fatalf("result = %v", result)
	}
	if g.analyzer.calls() != 1 {
		t.Fatalf("analyzer calls = %d", g.analyzer.calls())
	}
}

// TestAsyncFlow is US-302 AC8 at unit level: POST → 202 → poll → 200 with results.
func TestAsyncFlow(t *testing.T) {
	g := newGraphHarness(t, okResult)
	rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(asyncMinNodes))
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d %s", rec.Code, rec.Body.String())
	}
	data := decode(t, rec)["data"].(map[string]any)
	jobID := data["job_id"].(string)
	if rec.Header().Get("Location") != "/v1/graph/jobs/"+jobID || data["status"] != "queued" {
		t.Fatalf("202 body = %v, Location = %q", data, rec.Header().Get("Location"))
	}

	var job map[string]any
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		poll := g.do(http.MethodGet, "/v1/graph/jobs/"+jobID)
		job = decode(t, poll)["data"].(map[string]any)
		if job["status"] == "succeeded" {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if job["status"] != "succeeded" || job["result"] == nil || job["completed_at"] == nil {
		t.Fatalf("job = %v", job)
	}
	if size := job["graph_size"].(map[string]any); size["nodes"] != float64(asyncMinNodes) || size["edges"] != float64(asyncMinNodes-1) {
		t.Fatalf("graph_size = %v", size)
	}

	list := decode(t, g.do(http.MethodGet, "/v1/graph/jobs"))["data"].([]any)
	if len(list) != 1 || list[0].(map[string]any)["job_id"] != jobID || list[0].(map[string]any)["result"] != nil {
		t.Fatalf("job list = %v (US-302 AC6: status and times, without results)", list)
	}
}

func TestAsyncJobFailureIsReportedOnThePoll(t *testing.T) {
	g := newGraphHarness(t, func(context.Context) (json.RawMessage, error) {
		return nil, &eopclient.AnalysisError{Code: "ERROR_GRAPH_TOO_LARGE", Message: "too many edges"}
	})
	jobID := decode(t, g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(asyncMinNodes)))["data"].(map[string]any)["job_id"].(string)
	var job map[string]any
	for i := 0; i < 500 && (job == nil || job["status"] != "failed"); i++ {
		job = decode(t, g.do(http.MethodGet, "/v1/graph/jobs/"+jobID))["data"].(map[string]any)
		time.Sleep(10 * time.Millisecond)
	}
	if errBody, _ := job["error"].(map[string]any); job["status"] != "failed" || errBody["code"] != analysis.CodeGraphTooLarge {
		t.Fatalf("job = %v", job)
	}
}

func TestAnalyzeValidationNamesTheField(t *testing.T) {
	g := newGraphHarness(t, okResult)
	tests := map[string]struct{ body, field, code string }{
		"empty body":        {``, "body", CodeInvalidBody},
		"not json":          {`{"nodes":`, "body", CodeInvalidBody},
		"two objects":       {`{"nodes":["A"],"edges":[],"algorithms":["centrality"]}{}`, "body", CodeInvalidBody},
		"wrong type":        {`{"nodes":"A","edges":[],"algorithms":["centrality"]}`, "nodes", CodeInvalidBody},
		"unknown field":     {`{"nodes":["A"],"edges":[],"algorithms":["centrality"],"algorithm":"x"}`, "algorithm", CodeInvalidBody},
		"missing edges":     {`{"nodes":["A"],"algorithms":["centrality"]}`, "edges", analysis.CodeInvalidGraph},
		"edge to nowhere":   {`{"nodes":["A","B"],"edges":[{"from":"A","to":"Z","weight":1}],"algorithms":["centrality"]}`, "edges[0].to", analysis.CodeInvalidGraph},
		"bad weight":        {`{"nodes":["A","B"],"edges":[{"from":"A","to":"B","weight":-1}],"algorithms":["centrality"]}`, "edges[0].weight", analysis.CodeInvalidGraph},
		"unknown algorithm": {`{"nodes":["A"],"edges":[],"algorithms":["pagerank"]}`, "algorithms[0]", analysis.CodeUnsupportedAlgorithm},
	}
	for name, tt := range tests {
		t.Run(name, func(t *testing.T) {
			rec := g.doBody(http.MethodPost, "/v1/graph/analyze", tt.body)
			errBody := decode(t, rec)["error"].(map[string]any)
			if rec.Code != http.StatusBadRequest || errBody["field"] != tt.field || errBody["code"] != tt.code {
				t.Fatalf("got %d %v, want field %s code %s", rec.Code, errBody, tt.field, tt.code)
			}
		})
	}
	if g.analyzer.calls() != 0 {
		t.Fatal("invalid requests must not reach the engine")
	}
}

func TestAnalyzeBodyTooLarge(t *testing.T) {
	g := newGraphHarness(t, okResult)
	rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(400)) // > 4096 bytes
	if rec.Code != http.StatusRequestEntityTooLarge || errorCode(t, rec) != CodePayloadTooLarge {
		t.Fatalf("got %d %s", rec.Code, rec.Body.String())
	}
}

func TestSyncAnalysisErrors(t *testing.T) {
	tests := map[string]struct {
		err    error
		status int
		code   string
	}{
		"engine offline (AC3)": {&eopclient.ProtocolError{Code: eopclient.CodeServiceUnavailable, Desc: "HPC engine unavailable"}, http.StatusServiceUnavailable, analysis.CodeHPCUnavailable},
		"engine rejects graph": {&eopclient.AnalysisError{Code: "ERROR_INVALID_GRAPH", Message: "self loop", Field: "payload.edges[0]"}, http.StatusBadRequest, analysis.CodeInvalidGraph},
		"too large":            {&eopclient.AnalysisError{Code: "ERROR_GRAPH_TOO_LARGE", Message: "limit"}, http.StatusRequestEntityTooLarge, analysis.CodeGraphTooLarge},
		"engine crashed":       {&eopclient.AnalysisError{Code: "ERROR_ANALYSIS_FAILED", Message: "segfault in centrality.cpp"}, http.StatusBadGateway, analysis.CodeAnalysisFailed},
	}
	for name, tt := range tests {
		t.Run(name, func(t *testing.T) {
			g := newGraphHarness(t, func(context.Context) (json.RawMessage, error) { return nil, tt.err })
			rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(2))
			if rec.Code != tt.status || errorCode(t, rec) != tt.code {
				t.Fatalf("got %d %s", rec.Code, rec.Body.String())
			}
			for _, leak := range []string{"HPC engine unavailable", "centrality.cpp"} {
				if strings.Contains(rec.Body.String(), leak) {
					t.Fatalf("internal detail %q exposed", leak)
				}
			}
		})
	}
}

func TestSyncAnalysisTimesOut(t *testing.T) {
	g := newGraphHarness(t, func(ctx context.Context) (json.RawMessage, error) {
		<-ctx.Done()
		return nil, ctx.Err()
	})
	g.server.analysisOpts.SyncTimeout = 50 * time.Millisecond
	rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(2))
	if rec.Code != http.StatusGatewayTimeout || errorCode(t, rec) != analysis.CodeAnalysisTimeout {
		t.Fatalf("got %d %s", rec.Code, rec.Body.String())
	}
}

func TestAsyncRequiresARegisteredEngine(t *testing.T) {
	g := newGraphHarness(t, okResult)
	g.store.nodes = []eopclient.Node{{NodeID: "hpc-engine-1", Status: "OFFLINE"}, {NodeID: "vault-101", Status: "ONLINE"}}
	rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(asyncMinNodes))
	if rec.Code != http.StatusServiceUnavailable || errorCode(t, rec) != analysis.CodeHPCUnavailable {
		t.Fatalf("got %d %s", rec.Code, rec.Body.String())
	}

	g.store.nodes = []eopclient.Node{{NodeID: "hpc-engine-2", Status: "ONLINE"}} // replica naming (ADR-022)
	if rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(asyncMinNodes)); rec.Code != http.StatusAccepted {
		t.Fatalf("engine replica not recognized: %d", rec.Code)
	}

	g.store.err = &eopclient.UnavailableError{Err: errors.New("refused")}
	if rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(asyncMinNodes)); rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("eop-server down: %d", rec.Code)
	}
}

func TestAsyncQueueFullAndShutdown(t *testing.T) {
	release := make(chan struct{})
	g := newGraphHarness(t, func(context.Context) (json.RawMessage, error) {
		<-release
		return okResult(context.Background())
	})
	t.Cleanup(func() { close(release) })
	codes := []int{}
	for i := 0; i < 5; i++ { // 1 running + 2 queued, then full
		codes = append(codes, g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(asyncMinNodes)).Code)
		time.Sleep(20 * time.Millisecond)
	}
	last := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(asyncMinNodes))
	if last.Code != http.StatusTooManyRequests || errorCode(t, last) != CodeJobQueueFull || last.Header().Get("Retry-After") == "" {
		t.Fatalf("codes = %v, last = %d %s", codes, last.Code, last.Body.String())
	}

	go func() { _ = g.runner.Stop(context.Background()) }()
	time.Sleep(20 * time.Millisecond)
	if rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(asyncMinNodes)); rec.Code != http.StatusServiceUnavailable || errorCode(t, rec) != analysis.CodeGatewayShutdown {
		t.Fatalf("after Stop: %d %s", rec.Code, rec.Body.String())
	}
}

func TestJobLookups(t *testing.T) {
	g := newGraphHarness(t, okResult)
	if rec := g.do(http.MethodGet, "/v1/graph/jobs/not-a-uuid"); rec.Code != http.StatusBadRequest || errorCode(t, rec) != CodeInvalidJobID {
		t.Fatalf("invalid id: %d %s", rec.Code, rec.Body.String())
	}
	if rec := g.do(http.MethodGet, "/v1/graph/jobs/5f0c6f4e-0d1a-4a8e-9a56-3c7b1d2e4f60"); rec.Code != http.StatusNotFound || errorCode(t, rec) != CodeJobNotFound {
		t.Fatalf("unknown id: %d %s", rec.Code, rec.Body.String())
	}
	for _, limit := range []string{"0", "201", "x"} {
		if rec := g.do(http.MethodGet, "/v1/graph/jobs?limit="+limit); rec.Code != http.StatusBadRequest || errorCode(t, rec) != CodeInvalidLimit {
			t.Fatalf("limit=%s: %d", limit, rec.Code)
		}
	}
	if rec := g.do(http.MethodGet, "/v1/graph/jobs?limit=3"); rec.Code != http.StatusOK {
		t.Fatalf("limit=3: %d", rec.Code)
	}
}

func TestJobStoreDown(t *testing.T) {
	g := newGraphHarness(t, okResult)
	g.redis.Close()
	for _, rec := range []*httptestRecorder{
		g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(asyncMinNodes)),
		g.do(http.MethodGet, "/v1/graph/jobs"),
		g.do(http.MethodGet, "/v1/graph/jobs/5f0c6f4e-0d1a-4a8e-9a56-3c7b1d2e4f60"),
	} {
		if rec.Code != http.StatusServiceUnavailable || errorCode(t, rec) != CodeJobStoreUnavailable {
			t.Fatalf("got %d %s", rec.Code, rec.Body.String())
		}
	}
	// Synchronous analysis does not need Redis.
	if rec := g.doBody(http.MethodPost, "/v1/graph/analyze", graphBody(2)); rec.Code != http.StatusOK {
		t.Fatalf("sync analysis with Redis down: %d", rec.Code)
	}
	// Readiness stays green (eop-server is up) but reports the degraded dependency.
	health := decode(t, g.do(http.MethodGet, "/v1/health"))["data"].(map[string]any)
	if health["status"] != "degraded" || health["checks"].(map[string]any)["redis"] != "unavailable" {
		t.Fatalf("health = %v", health)
	}
}

func TestHealthReportsRedisWhenConfigured(t *testing.T) {
	g := newGraphHarness(t, okResult)
	health := decode(t, g.do(http.MethodGet, "/v1/health"))["data"].(map[string]any)
	if health["status"] != "ok" || health["checks"].(map[string]any)["redis"] != "ok" {
		t.Fatalf("health = %v", health)
	}
}

type httptestRecorder = httptest.ResponseRecorder

func telemetryDiscard() *slog.Logger { return telemetry.NewLogger(io.Discard, "api-gateway") }

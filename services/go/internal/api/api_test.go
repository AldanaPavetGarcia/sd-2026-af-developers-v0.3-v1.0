package api

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/getkin/kin-openapi/openapi3"
	"github.com/getkin/kin-openapi/openapi3filter"
	"github.com/getkin/kin-openapi/routers"
	"github.com/getkin/kin-openapi/routers/legacy"
	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus/testutil"
	"go.opentelemetry.io/otel"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"

	apispec "github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/api"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

type fakeStore struct {
	nodes        []eopclient.Node
	err          error
	heartbeatErr error
	queried      []string
	panicOnList  bool
}

func (f *fakeStore) ListNodes(context.Context) ([]eopclient.Node, error) {
	if f.panicOnList {
		panic("boom")
	}
	return f.nodes, f.err
}

func (f *fakeStore) QueryNode(_ context.Context, id string) (eopclient.Node, error) {
	f.queried = append(f.queried, id)
	if f.err != nil {
		return eopclient.Node{}, f.err
	}
	for _, n := range f.nodes {
		if n.NodeID == id {
			return n, nil
		}
	}
	return eopclient.Node{}, &eopclient.ProtocolError{Code: eopclient.CodeNotFound, Desc: "Node not found"}
}

func (f *fakeStore) Heartbeat(context.Context) error { return f.heartbeatErr }

var sampleNodes = []eopclient.Node{
	{NodeID: "alpha-01", BunkerName: "Zone-A", IPAddress: "10.0.0.5", Capacity: 80, Status: "ONLINE", LastSeen: time.Unix(1759700000, 0)},
	{NodeID: "beta-02", BunkerName: "Zone-B", IPAddress: "10.0.0.6", Capacity: 10, Status: "OFFLINE", LastSeen: time.Unix(1759700100, 0)},
}

type harness struct {
	t       *testing.T
	handler http.Handler
	server  *Server
	metrics *telemetry.Metrics
	logs    *bytes.Buffer
	router  routers.Router
	doc     *openapi3.T
}

func newHarness(t *testing.T, store NodeStore, mods ...func(*Options)) *harness {
	t.Helper()
	specJSON, err := apispec.JSON()
	if err != nil {
		t.Fatal(err)
	}
	doc, err := openapi3.NewLoader().LoadFromData(specJSON)
	if err != nil {
		t.Fatal(err)
	}
	router, err := legacy.NewRouter(doc)
	if err != nil {
		t.Fatal(err)
	}
	logs := &bytes.Buffer{}
	metrics := telemetry.NewMetrics("test", "stable")
	opts := Options{
		Store:         store,
		Logger:        telemetry.NewLogger(logs, "api-gateway"),
		Metrics:       metrics,
		OpenAPIJSON:   specJSON,
		HealthTimeout: time.Second,
	}
	for _, mod := range mods {
		mod(&opts)
	}
	server := NewServer(opts)
	return &harness{t: t, handler: server.Routes(), server: server, metrics: metrics, logs: logs, router: router, doc: doc}
}

// do serves one request and, for routes in the contract, validates the response against it.
func (h *harness) do(method, path string) *httptest.ResponseRecorder {
	h.t.Helper()
	return h.doBody(method, path, "")
}

func (h *harness) doBody(method, path, body string) *httptest.ResponseRecorder {
	h.t.Helper()
	req := httptest.NewRequest(method, path, strings.NewReader(body))
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	rec := httptest.NewRecorder()
	h.handler.ServeHTTP(rec, req)

	route, params, err := h.router.FindRoute(req)
	if err != nil {
		return rec // not part of the contract (unknown route, wrong method)
	}
	input := &openapi3filter.ResponseValidationInput{
		RequestValidationInput: &openapi3filter.RequestValidationInput{Request: req, PathParams: params, Route: route},
		Status:                 rec.Code,
		Header:                 rec.Header(),
		Body:                   io.NopCloser(bytes.NewReader(rec.Body.Bytes())),
		Options:                &openapi3filter.Options{IncludeResponseStatus: true},
	}
	if err := openapi3filter.ValidateResponse(context.Background(), input); err != nil {
		h.t.Fatalf("%s %s -> %d does not match the OpenAPI contract: %v\nbody: %s", method, path, rec.Code, err, rec.Body.String())
	}
	return rec
}

func decode(t *testing.T, rec *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	if ct := rec.Header().Get("Content-Type"); ct != "application/json" {
		t.Fatalf("Content-Type = %q, want application/json (US-301 AC3)", ct)
	}
	var body map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("invalid JSON: %v\n%s", err, rec.Body.String())
	}
	return body
}

func errorCode(t *testing.T, rec *httptest.ResponseRecorder) string {
	t.Helper()
	return decode(t, rec)["error"].(map[string]any)["code"].(string)
}

func TestListNodesEmptyRegistry(t *testing.T) {
	h := newHarness(t, &fakeStore{})
	rec := h.do(http.MethodGet, "/v1/nodes")
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	body := decode(t, rec)
	if data, ok := body["data"].([]any); !ok || len(data) != 0 {
		t.Fatalf("data = %#v, want an empty array (not null)", body["data"])
	}
	if _, ok := body["meta"].(map[string]any)["timestamp"]; !ok {
		t.Fatal("meta.timestamp missing (US-301 AC3)")
	}
}

func TestListNodesPopulated(t *testing.T) {
	h := newHarness(t, &fakeStore{nodes: sampleNodes})
	rec := h.do(http.MethodGet, "/v1/nodes")
	data := decode(t, rec)["data"].([]any)
	if len(data) != 2 {
		t.Fatalf("len(data) = %d", len(data))
	}
	first := data[0].(map[string]any)
	want := map[string]any{"node_id": "alpha-01", "bunker_name": "Zone-A", "status": "ONLINE", "ip_address": "10.0.0.5", "capacity": 80.0, "last_seen_at": "2025-10-05T21:33:20Z"}
	for key, value := range want {
		if first[key] != value {
			t.Errorf("%s = %v, want %v", key, first[key], value)
		}
	}
}

func TestGetNode(t *testing.T) {
	h := newHarness(t, &fakeStore{nodes: sampleNodes})
	rec := h.do(http.MethodGet, "/v1/nodes/beta-02")
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	if got := decode(t, rec)["data"].(map[string]any)["status"]; got != "OFFLINE" {
		t.Fatalf("status field = %v", got)
	}
}

func TestGetNodeNotFound(t *testing.T) {
	h := newHarness(t, &fakeStore{nodes: sampleNodes})
	rec := h.do(http.MethodGet, "/v1/nodes/ghost")
	if rec.Code != http.StatusNotFound || errorCode(t, rec) != CodeNodeNotFound {
		t.Fatalf("got %d %s", rec.Code, rec.Body.String())
	}
}

func TestGetNodeRejectsInvalidIDWithoutCallingTheServer(t *testing.T) {
	store := &fakeStore{nodes: sampleNodes}
	h := newHarness(t, store)
	for _, path := range []string{"/v1/nodes/bad%20id", "/v1/nodes/" + strings.Repeat("x", 64), "/v1/nodes/a%0ALIST_NODES"} {
		rec := h.do(http.MethodGet, path)
		body := decode(t, rec)
		errBody := body["error"].(map[string]any)
		if rec.Code != http.StatusBadRequest || errBody["code"] != CodeInvalidNodeID || errBody["field"] != "node_id" {
			t.Fatalf("%s: got %d %s", path, rec.Code, rec.Body.String())
		}
	}
	if len(store.queried) != 0 {
		t.Fatalf("invalid ids reached the store: %v", store.queried)
	}
}

func TestUpstreamFailuresDoNotLeakDetails(t *testing.T) {
	tests := map[string]struct {
		err        error
		wantStatus int
		wantCode   string
	}{
		"unreachable": {&eopclient.UnavailableError{Err: errors.New("dial tcp 10.1.2.3:9026: connection refused")}, http.StatusServiceUnavailable, CodeUpstreamUnavailable},
		"timeout":     {context.DeadlineExceeded, http.StatusServiceUnavailable, CodeUpstreamUnavailable},
		"bad answer":  {&eopclient.MalformedResponseError{Line: "garbage", Reason: "x"}, http.StatusBadGateway, CodeUpstreamError},
		"server err":  {&eopclient.ProtocolError{Code: "ERROR_BAD_REQUEST", Desc: "Malformed QUERY_NODE"}, http.StatusBadGateway, CodeUpstreamError},
	}
	for name, tt := range tests {
		t.Run(name, func(t *testing.T) {
			h := newHarness(t, &fakeStore{err: tt.err})
			for _, path := range []string{"/v1/nodes", "/v1/nodes/alpha-01"} {
				rec := h.do(http.MethodGet, path)
				if rec.Code != tt.wantStatus || errorCode(t, rec) != tt.wantCode {
					t.Fatalf("%s: got %d %s", path, rec.Code, rec.Body.String())
				}
				for _, leak := range []string{"10.1.2.3", "garbage", "Malformed QUERY_NODE"} {
					if strings.Contains(rec.Body.String(), leak) {
						t.Fatalf("%s: response exposes internal detail %q", path, leak)
					}
				}
			}
		})
	}
}

func TestHealth(t *testing.T) {
	h := newHarness(t, &fakeStore{})
	rec := h.do(http.MethodGet, "/v1/health")
	if rec.Code != http.StatusOK || decode(t, rec)["data"].(map[string]any)["status"] != "ok" {
		t.Fatalf("got %d %s", rec.Code, rec.Body.String())
	}

	down := newHarness(t, &fakeStore{heartbeatErr: &eopclient.UnavailableError{Err: errors.New("refused")}})
	rec = down.do(http.MethodGet, "/v1/health")
	if rec.Code != http.StatusServiceUnavailable || errorCode(t, rec) != CodeUpstreamUnavailable {
		t.Fatalf("got %d %s", rec.Code, rec.Body.String())
	}
}

func TestOpenAPIDocumentIsServedAndValid(t *testing.T) {
	h := newHarness(t, &fakeStore{})
	rec := h.do(http.MethodGet, "/v1/openapi.json")
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	doc, err := openapi3.NewLoader().LoadFromData(rec.Body.Bytes())
	if err != nil {
		t.Fatalf("served document does not load: %v", err)
	}
	if err := doc.Validate(context.Background()); err != nil {
		t.Fatalf("served document is not valid OpenAPI 3: %v", err)
	}
	if !strings.HasPrefix(doc.OpenAPI, "3.0") {
		t.Fatalf("openapi = %q, want 3.0.x (US-301 AC4)", doc.OpenAPI)
	}
}

// TestEveryRouteIsInTheContract fails when a route is added without documenting it, and when the
// contract documents a route that does not exist.
func TestEveryRouteIsInTheContract(t *testing.T) {
	h := newHarness(t, &fakeStore{})
	served := map[string]bool{}
	err := chi.Walk(h.server.router(), func(method, route string, _ http.Handler, _ ...func(http.Handler) http.Handler) error {
		served[method+" "+strings.TrimSuffix(route, "/")] = true
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	documented := map[string]bool{}
	for path, item := range h.doc.Paths.Map() {
		for method := range item.Operations() {
			documented[method+" "+path] = true
		}
	}
	for route := range served {
		if !documented[route] {
			t.Errorf("route %s is served but missing from openapi.yaml", route)
		}
	}
	for route := range documented {
		if !served[route] {
			t.Errorf("route %s is documented but not served", route)
		}
	}
}

func TestEveryRouteIsVersioned(t *testing.T) {
	h := newHarness(t, &fakeStore{})
	for path := range h.doc.Paths.Map() {
		if !strings.HasPrefix(path, "/v1/") {
			t.Errorf("path %s is not under /v1/ (US-301 AC5)", path)
		}
	}
}

func TestUnknownRouteAndMethodUseTheErrorEnvelope(t *testing.T) {
	h := newHarness(t, &fakeStore{})
	if rec := h.do(http.MethodGet, "/v2/nodes"); rec.Code != http.StatusNotFound || errorCode(t, rec) != CodeRouteNotFound {
		t.Fatalf("unknown route: %d %s", rec.Code, rec.Body.String())
	}
	if rec := h.do(http.MethodPost, "/v1/nodes"); rec.Code != http.StatusMethodNotAllowed || errorCode(t, rec) != CodeMethodNotAllowed {
		t.Fatalf("wrong method: %d %s", rec.Code, rec.Body.String())
	}
}

func TestPanicBecomesInternalError(t *testing.T) {
	h := newHarness(t, &fakeStore{panicOnList: true})
	rec := h.do(http.MethodGet, "/v1/nodes")
	if rec.Code != http.StatusInternalServerError || errorCode(t, rec) != CodeInternal {
		t.Fatalf("got %d %s", rec.Code, rec.Body.String())
	}
	if got := testutil.ToFloat64(h.metrics.RequestCounter("GET", "/v1/nodes", "5xx")); got != 1 {
		t.Fatalf("recovered panic counted %v times as 5xx, want 1", got)
	}
}

// ADR-011: during a canary or blue-green switch the load test attributes each response to a
// deployment by these headers, so they must be on every response, errors and panics included.
func TestResponsesNameTheServingDeployment(t *testing.T) {
	h := newHarness(t, &fakeStore{nodes: sampleNodes, panicOnList: true}, func(o *Options) {
		o.Version, o.Track = "0.3.1", "canary"
	})
	for _, path := range []string{"/v1/nodes/alpha-01", "/v1/nodes/ghost", "/v2/nodes", "/v1/nodes"} {
		rec := h.do(http.MethodGet, path)
		if got := rec.Header().Get(HeaderVersion); got != "0.3.1" {
			t.Errorf("%s (%d): %s = %q, want 0.3.1", path, rec.Code, HeaderVersion, got)
		}
		if got := rec.Header().Get(HeaderTrack); got != "canary" {
			t.Errorf("%s (%d): %s = %q, want canary", path, rec.Code, HeaderTrack, got)
		}
	}
}

func TestVersionHeadersAreOmittedWhenUnset(t *testing.T) {
	rec := newHarness(t, &fakeStore{}).do(http.MethodGet, "/v1/nodes")
	if rec.Header().Get(HeaderVersion) != "" || rec.Header().Get(HeaderTrack) != "" {
		t.Fatalf("unexpected headers: %v", rec.Header())
	}
}

func TestMetricsUseRoutePatternNotRawPath(t *testing.T) {
	h := newHarness(t, &fakeStore{nodes: sampleNodes})
	h.do(http.MethodGet, "/v1/nodes/alpha-01")
	h.do(http.MethodGet, "/v1/nodes/ghost")
	if got := testutil.ToFloat64(h.metrics.RequestCounter("GET", "/v1/nodes/{node_id}", "2xx")); got != 1 {
		t.Fatalf("2xx count = %v", got)
	}
	if got := testutil.ToFloat64(h.metrics.RequestCounter("GET", "/v1/nodes/{node_id}", "4xx")); got != 1 {
		t.Fatalf("4xx count = %v", got)
	}
}

func TestTraceIDInMetaAndLogs(t *testing.T) {
	provider := sdktrace.NewTracerProvider()
	previous := otel.GetTracerProvider()
	otel.SetTracerProvider(provider)
	t.Cleanup(func() {
		otel.SetTracerProvider(previous)
		_ = provider.Shutdown(context.Background())
	})

	h := newHarness(t, &fakeStore{nodes: sampleNodes})
	rec := h.do(http.MethodGet, "/v1/nodes")
	traceID, _ := decode(t, rec)["meta"].(map[string]any)["trace_id"].(string)
	if len(traceID) != 32 {
		t.Fatalf("meta.trace_id = %q, want a 32-hex trace id", traceID)
	}

	var line map[string]any
	if err := json.Unmarshal(bytes.TrimSpace(h.logs.Bytes()), &line); err != nil {
		t.Fatalf("access log is not one JSON line: %v\n%s", err, h.logs.String())
	}
	for key, want := range map[string]any{"trace_id": traceID, "service": "api-gateway", "route": "/v1/nodes", "message": "request served", "level": slog.LevelInfo.String()} {
		if line[key] != want {
			t.Errorf("log %s = %v, want %v", key, line[key], want)
		}
	}
}

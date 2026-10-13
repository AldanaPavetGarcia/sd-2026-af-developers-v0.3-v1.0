package api

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/breaker"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/external"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/stub"
)

type externalHarness struct {
	*harness
	ai, corp       *stub.Stub
	aiSvc, corpSvc *external.Service
	aiURL, corpURL string
	store          *fakeStore
}

func newExternalHarness(t *testing.T) *externalHarness {
	t.Helper()
	settings := breaker.Defaults()
	settings.Breaker.FailureThreshold = 3
	settings.Breaker.ResetTimeout = 150 * time.Millisecond
	settings.CallTimeout = 200 * time.Millisecond

	e := &externalHarness{store: &fakeStore{nodes: sampleNodes}}
	var err error
	e.ai, _ = stub.New(stub.KindAI, stub.FaultNone, time.Second)
	e.corp, _ = stub.New(stub.KindCorporate, stub.FaultNone, time.Second)
	aiSrv, corpSrv := httptest.NewServer(e.ai.Handler()), httptest.NewServer(e.corp.Handler())
	t.Cleanup(aiSrv.Close)
	t.Cleanup(corpSrv.Close)
	e.aiURL, e.corpURL = aiSrv.URL, corpSrv.URL
	if e.aiSvc, err = external.New("ai", aiSrv.URL, settings, 10, external.Hooks{}); err != nil {
		t.Fatal(err)
	}
	if e.corpSvc, err = external.New("corporate", corpSrv.URL, settings, 10, external.Hooks{}); err != nil {
		t.Fatal(err)
	}
	e.harness = newHarness(t, e.store, func(o *Options) { o.AI, o.Corporate = e.aiSvc, e.corpSvc })
	return e
}

func setFault(t *testing.T, url, mode string) {
	t.Helper()
	resp, err := http.Post(url+"/admin/fault", "application/json", strings.NewReader(`{"mode":"`+mode+`"}`))
	if err != nil || resp.StatusCode != http.StatusNoContent {
		t.Fatalf("set fault %s: %v %v", mode, resp, err)
	}
	_ = resp.Body.Close()
}

func TestInsightsFreshThenStaleThenDegraded(t *testing.T) {
	e := newExternalHarness(t)
	rec := e.do(http.MethodGet, "/v1/nodes/alpha-01/insights")
	body := decode(t, rec)
	if rec.Code != http.StatusOK || body["data"].(map[string]any)["node_id"] != "alpha-01" || body["meta"].(map[string]any)["stale"] != nil {
		t.Fatalf("fresh: %d %v", rec.Code, body)
	}

	setFault(t, e.aiURL, stub.FaultError)
	rec = e.do(http.MethodGet, "/v1/nodes/alpha-01/insights")
	body = decode(t, rec)
	if m := body["meta"].(map[string]any); rec.Code != http.StatusOK || m["stale"] != true || m["stale_since"] == nil {
		t.Fatalf("cached fallback: %d %v", rec.Code, body)
	}

	rec = e.do(http.MethodGet, "/v1/nodes/beta-02/insights") // never fetched: nothing cached
	if rec.Code != http.StatusServiceUnavailable || errorCode(t, rec) != CodeServiceDegraded || rec.Header().Get("Retry-After") == "" {
		t.Fatalf("degraded: %d %s", rec.Code, rec.Body.String())
	}
}

// TestCircuitBreakerDemo is US-303 AC7 at the API level: service down → circuit opens after the
// threshold → fallback served → service recovers → circuit closes → normal response.
func TestCircuitBreakerDemo(t *testing.T) {
	e := newExternalHarness(t)
	_ = e.do(http.MethodGet, "/v1/nodes/alpha-01/insights") // cache a good answer
	setFault(t, e.aiURL, stub.FaultError)
	for i := 0; i < 3; i++ {
		_ = e.do(http.MethodGet, "/v1/nodes/alpha-01/insights")
	}
	if e.aiSvc.Breaker().State() != breaker.Open {
		t.Fatalf("state = %v after the failure threshold", e.aiSvc.Breaker().State())
	}
	if rec := e.do(http.MethodGet, "/v1/nodes/alpha-01/insights"); rec.Code != http.StatusOK || decode(t, rec)["meta"].(map[string]any)["stale"] != true {
		t.Fatalf("open circuit must serve the fallback: %d %s", rec.Code, rec.Body.String())
	}
	if e.corpSvc.Breaker().State() != breaker.Closed {
		t.Fatal("one circuit per service: corporate must stay closed (US-303 AC1)")
	}

	setFault(t, e.aiURL, stub.FaultNone)
	time.Sleep(200 * time.Millisecond) // reset timeout → half-open
	rec := e.do(http.MethodGet, "/v1/nodes/alpha-01/insights")
	if rec.Code != http.StatusOK || decode(t, rec)["meta"].(map[string]any)["stale"] != nil || e.aiSvc.Breaker().State() != breaker.Closed {
		t.Fatalf("after recovery: %d %s, state %v", rec.Code, rec.Body.String(), e.aiSvc.Breaker().State())
	}
}

func TestCorporateUsesTheNodesBunker(t *testing.T) {
	e := newExternalHarness(t)
	rec := e.do(http.MethodGet, "/v1/nodes/beta-02/corporate")
	if data := decode(t, rec)["data"].(map[string]any); rec.Code != http.StatusOK || data["bunker_name"] != "Zone-B" {
		t.Fatalf("got %d %v", rec.Code, data)
	}
	if rec := e.do(http.MethodGet, "/v1/nodes/ghost/corporate"); rec.Code != http.StatusNotFound || errorCode(t, rec) != CodeNodeNotFound {
		t.Fatalf("unknown node: %d %s", rec.Code, rec.Body.String())
	}
	e.store.err = &eopclient.UnavailableError{Err: errors.New("refused")}
	if rec := e.do(http.MethodGet, "/v1/nodes/beta-02/corporate"); rec.Code != http.StatusServiceUnavailable || errorCode(t, rec) != CodeUpstreamUnavailable {
		t.Fatalf("registry down: %d %s", rec.Code, rec.Body.String())
	}
}

func TestExternalRoutesValidateTheNodeID(t *testing.T) {
	e := newExternalHarness(t)
	for _, path := range []string{"/v1/nodes/bad%20id/insights", "/v1/nodes/bad%20id/corporate"} {
		if rec := e.do(http.MethodGet, path); rec.Code != http.StatusBadRequest || errorCode(t, rec) != CodeInvalidNodeID {
			t.Fatalf("%s: %d", path, rec.Code)
		}
	}
}

// fixedExternal returns a canned outcome, for the error branches the stub cannot produce.
type fixedExternal struct{ err error }

func (f fixedExternal) Name() string { return "ai" }
func (f fixedExternal) Get(context.Context, string, string) (external.Response, error) {
	return external.Response{}, f.err
}

func TestExternalErrorMapping(t *testing.T) {
	tests := map[string]struct {
		err    error
		status int
		code   string
	}{
		"not found": {external.ErrNotFound, http.StatusNotFound, CodeExternalMissing},
		"rejected":  {&external.RejectedError{Service: "ai", Status: 422}, http.StatusBadGateway, CodeUpstreamError},
		"cancelled": {context.Canceled, http.StatusServiceUnavailable, CodeServiceDegraded},
	}
	for name, tt := range tests {
		t.Run(name, func(t *testing.T) {
			h := newHarness(t, &fakeStore{nodes: sampleNodes}, func(o *Options) { o.AI = fixedExternal{tt.err} })
			if rec := h.do(http.MethodGet, "/v1/nodes/alpha-01/insights"); rec.Code != tt.status || errorCode(t, rec) != tt.code {
				t.Fatalf("got %d %s", rec.Code, rec.Body.String())
			}
		})
	}
}

package stub

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func serve(h http.Handler, method, path, body string) *httptest.ResponseRecorder {
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(method, path, strings.NewReader(body)))
	return rec
}

func TestAIInsightsAreDeterministic(t *testing.T) {
	s, _ := New(KindAI, FaultNone, time.Second)
	h := s.Handler()
	first := serve(h, "GET", "/v1/insights/vault-101", "")
	second := serve(h, "GET", "/v1/insights/vault-101", "")
	if first.Code != http.StatusOK || first.Body.String() != second.Body.String() {
		t.Fatalf("got %d %s / %s", first.Code, first.Body, second.Body)
	}
	var body map[string]any
	_ = json.Unmarshal(first.Body.Bytes(), &body)
	if body["node_id"] != "vault-101" || body["risk_score"].(float64) < 0 || body["risk_score"].(float64) >= 1 || body["recommendation"] == "" {
		t.Fatalf("body = %v", body)
	}
	if serve(h, "GET", "/v1/bunkers/Zone-A/status", "").Code != http.StatusNotFound {
		t.Fatal("the AI stub must not serve the corporate endpoint")
	}
}

func TestHighRiskRecommendation(t *testing.T) {
	s, _ := New(KindAI, FaultNone, time.Second)
	h := s.Handler()
	seen := map[string]bool{}
	for i := 0; i < 200; i++ {
		var body map[string]any
		_ = json.Unmarshal(serve(h, "GET", "/v1/insights/n"+string(rune('a'+i%26))+string(rune('a'+i/26)), "").Body.Bytes(), &body)
		seen[body["recommendation"].(string)] = true
	}
	if len(seen) != 2 {
		t.Fatalf("recommendations = %v, want both outcomes across nodes", seen)
	}
}

func TestCorporateStatus(t *testing.T) {
	s, _ := New(KindCorporate, FaultNone, time.Second)
	rec := serve(s.Handler(), "GET", "/v1/bunkers/Zone-B/status", "")
	var body map[string]any
	_ = json.Unmarshal(rec.Body.Bytes(), &body)
	if rec.Code != http.StatusOK || body["bunker_name"] != "Zone-B" || body["supply_quota"] == nil || body["compliant"] == nil {
		t.Fatalf("got %d %v", rec.Code, body)
	}
}

func TestFaultInjection(t *testing.T) {
	s, _ := New(KindAI, FaultNone, 20*time.Millisecond)
	h := s.Handler()
	if rec := serve(h, "POST", "/admin/fault", `{"mode":"error"}`); rec.Code != http.StatusNoContent {
		t.Fatalf("set fault: %d", rec.Code)
	}
	if rec := serve(h, "GET", "/v1/insights/a", ""); rec.Code != http.StatusInternalServerError {
		t.Fatalf("error mode: %d", rec.Code)
	}
	if rec := serve(h, "GET", "/health", ""); rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"fault":"error"`) {
		t.Fatalf("health must stay up and report the fault: %d %s", rec.Code, rec.Body)
	}
	_ = serve(h, "POST", "/admin/fault", `{"mode":"slow"}`)
	start := time.Now()
	if rec := serve(h, "GET", "/v1/insights/a", ""); rec.Code != http.StatusOK || time.Since(start) < 20*time.Millisecond {
		t.Fatalf("slow mode: %d after %v", rec.Code, time.Since(start))
	}
	if rec := serve(h, "GET", "/admin/fault", ""); !strings.Contains(rec.Body.String(), `"mode":"slow"`) {
		t.Fatalf("GET fault = %s", rec.Body)
	}
	for _, bad := range []string{`{"mode":"broken"}`, `not json`} {
		if rec := serve(h, "POST", "/admin/fault", bad); rec.Code != http.StatusBadRequest {
			t.Fatalf("%s: %d", bad, rec.Code)
		}
	}
}

func TestSlowModeHonoursCancellation(t *testing.T) {
	s, _ := New(KindAI, FaultSlow, time.Hour)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	req := httptest.NewRequest("GET", "/v1/insights/a", nil).WithContext(ctx)
	rec := httptest.NewRecorder()
	done := make(chan struct{})
	go func() { s.Handler().ServeHTTP(rec, req); close(done) }()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("a cancelled request kept waiting in slow mode")
	}
}

func TestNewRejectsUnknownKindsAndFaults(t *testing.T) {
	if _, err := New("billing", FaultNone, time.Second); err == nil {
		t.Fatal("unknown kind accepted")
	}
	if _, err := New(KindAI, "flaky", time.Second); err == nil {
		t.Fatal("unknown fault accepted")
	}
}

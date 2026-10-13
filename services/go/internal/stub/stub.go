// Package stub stands in for Vault-Tec's external AI and corporate services (ADR-012 §1). It is
// a test double: answers are deterministic, and failures are injected on demand so the circuit
// breaker can be demonstrated (US-303 AC7).
package stub

import (
	"encoding/json"
	"errors"
	"hash/fnv"
	"net/http"
	"sync"
	"time"
)

// Kinds of external service the stub can play.
const (
	KindAI        = "ai"
	KindCorporate = "corporate"
)

// Fault modes.
const (
	FaultNone  = "ok"
	FaultError = "error"
	FaultSlow  = "slow"
)

const (
	riskScale       = 1000
	highRisk        = 0.7
	maxSupplyQuota  = 500
	complianceRatio = 5 // one bunker in five is not compliant
)

// Stub is one fake external service.
type Stub struct {
	kind      string
	slowDelay time.Duration

	mu    sync.Mutex
	fault string
}

// New returns a stub of the given kind ("ai" or "corporate").
func New(kind, fault string, slowDelay time.Duration) (*Stub, error) {
	if kind != KindAI && kind != KindCorporate {
		return nil, errors.New(`stub: kind must be "ai" or "corporate"`)
	}
	s := &Stub{kind: kind, slowDelay: slowDelay}
	if err := s.setFault(fault); err != nil {
		return nil, err
	}
	return s, nil
}

func (s *Stub) setFault(mode string) error {
	switch mode {
	case FaultNone, FaultError, FaultSlow:
		s.mu.Lock()
		s.fault = mode
		s.mu.Unlock()
		return nil
	}
	return errors.New(`stub: fault must be "ok", "error" or "slow"`)
}

func (s *Stub) currentFault() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.fault
}

// Handler serves /health, the business endpoint of the stub's kind and /admin/fault.
func (s *Stub) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "kind": s.kind, "fault": s.currentFault()})
	})
	mux.HandleFunc("GET /admin/fault", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"mode": s.currentFault()})
	})
	mux.HandleFunc("POST /admin/fault", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Mode string `json:"mode"`
		}
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&body); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "body must be {\"mode\": \"ok|error|slow\"}"})
			return
		}
		if err := s.setFault(body.Mode); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})
	if s.kind == KindAI {
		mux.HandleFunc("GET /v1/insights/{node_id}", s.faulty(s.insights))
	} else {
		mux.HandleFunc("GET /v1/bunkers/{bunker_name}/status", s.faulty(s.bunkerStatus))
	}
	return mux
}

// faulty applies the injected fault before the business handler.
func (s *Stub) faulty(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		switch s.currentFault() {
		case FaultError:
			writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "injected failure"})
			return
		case FaultSlow:
			select {
			case <-time.After(s.slowDelay):
			case <-r.Context().Done():
				return
			}
		}
		next(w, r)
	}
}

func (s *Stub) insights(w http.ResponseWriter, r *http.Request) {
	nodeID := r.PathValue("node_id")
	risk := float64(hash(nodeID)%riskScale) / riskScale
	recommendation := "no action needed"
	if risk >= highRisk {
		recommendation = "schedule maintenance within 24 h"
	}
	writeJSON(w, http.StatusOK, map[string]any{"node_id": nodeID, "risk_score": risk, "recommendation": recommendation})
}

func (s *Stub) bunkerStatus(w http.ResponseWriter, r *http.Request) {
	bunker := r.PathValue("bunker_name")
	h := hash(bunker)
	writeJSON(w, http.StatusOK, map[string]any{
		"bunker_name": bunker, "supply_quota": int(h % maxSupplyQuota), "compliant": h%complianceRatio != 0,
	})
}

func hash(s string) uint32 {
	h := fnv.New32a()
	_, _ = h.Write([]byte(s))
	return h.Sum32()
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

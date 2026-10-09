package api

import (
	"context"
	"errors"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
)

const upstreamCheck = "eop-server"

// node is the public representation of a registry entry (US-301 AC1).
type node struct {
	NodeID     string `json:"node_id"`
	BunkerName string `json:"bunker_name"`
	Status     string `json:"status"`
	IPAddress  string `json:"ip_address"`
	Capacity   int    `json:"capacity"`
	LastSeenAt string `json:"last_seen_at"`
}

func toNode(n eopclient.Node) node {
	return node{
		NodeID:     n.NodeID,
		BunkerName: n.BunkerName,
		Status:     n.Status,
		IPAddress:  n.IPAddress,
		Capacity:   n.Capacity,
		LastSeenAt: n.LastSeen.UTC().Format(time.RFC3339),
	}
}

func (s *Server) listNodes(w http.ResponseWriter, r *http.Request) {
	records, err := s.store.ListNodes(r.Context())
	if err != nil {
		s.writeStoreError(w, r, err)
		return
	}
	nodes := make([]node, 0, len(records))
	for _, record := range records {
		nodes = append(nodes, toNode(record))
	}
	s.writeData(w, r, http.StatusOK, nodes)
}

func (s *Server) getNode(w http.ResponseWriter, r *http.Request) {
	nodeID := chi.URLParam(r, "node_id")
	if !eopclient.ValidNodeID(nodeID) {
		s.writeError(w, r, http.StatusBadRequest, errorBody{
			Code:    CodeInvalidNodeID,
			Message: "node_id must be 1 to 63 characters from A-Z, a-z, 0-9, '.', '_' and '-'",
			Field:   "node_id",
		})
		return
	}
	record, err := s.store.QueryNode(r.Context(), nodeID)
	if err != nil {
		s.writeStoreError(w, r, err)
		return
	}
	s.writeData(w, r, http.StatusOK, toNode(record))
}

const (
	redisCheck     = "redis"
	checkOK        = "ok"
	checkFailed    = "unavailable"
	statusReady    = "ok"
	statusDegraded = "degraded"
)

type healthStatus struct {
	Status string            `json:"status"`
	Checks map[string]string `json:"checks"`
}

// health is the readiness probe (US-301 AC7). Only eop-server is critical: without Redis the
// node routes and synchronous analysis still work, so the pod stays ready and reports
// "degraded" instead of leaving the Service (ADR-010 §3).
func (s *Server) health(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), s.healthTimeout)
	defer cancel()
	if err := s.store.Heartbeat(ctx); err != nil {
		s.logger.WarnContext(r.Context(), "readiness check failed", "check", upstreamCheck, "error", err)
		s.writeError(w, r, http.StatusServiceUnavailable, errorBody{
			Code:    CodeUpstreamUnavailable,
			Message: upstreamCheck + " is not reachable",
		})
		return
	}
	status := healthStatus{Status: statusReady, Checks: map[string]string{upstreamCheck: checkOK}}
	if s.jobStore != nil {
		status.Checks[redisCheck] = checkOK
		if err := s.jobStore.Ping(ctx); err != nil {
			s.logger.WarnContext(r.Context(), "non-critical check failed", "check", redisCheck, "error", err)
			status.Checks[redisCheck] = checkFailed
			status.Status = statusDegraded
		}
	}
	s.writeData(w, r, http.StatusOK, status)
}

func (s *Server) openAPISpec(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", contentTypeJSON)
	if _, err := w.Write(s.openAPI); err != nil {
		s.logger.DebugContext(r.Context(), "writing OpenAPI document", "error", err)
	}
}

// writeStoreError maps eop-server failures to HTTP without exposing internal details.
func (s *Server) writeStoreError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, eopclient.ErrNotFound):
		s.writeError(w, r, http.StatusNotFound, errorBody{Code: CodeNodeNotFound, Message: "node not found"})
	case eopclient.IsUnavailable(err):
		s.logger.WarnContext(r.Context(), "eop-server unavailable", "error", err)
		s.writeError(w, r, http.StatusServiceUnavailable, errorBody{Code: CodeUpstreamUnavailable, Message: "the node registry is temporarily unavailable"})
	default:
		s.logger.ErrorContext(r.Context(), "unexpected eop-server answer", "error", err)
		s.writeError(w, r, http.StatusBadGateway, errorBody{Code: CodeUpstreamError, Message: "the node registry returned an unexpected answer"})
	}
}

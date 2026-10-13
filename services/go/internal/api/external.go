package api

import (
	"context"
	"errors"
	"math"
	"net/http"
	"net/url"
	"strconv"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/external"
)

// Error codes of the routes backed by external services.
const (
	CodeServiceDegraded = "SERVICE_DEGRADED"
	CodeExternalMissing = "EXTERNAL_NOT_FOUND"
)

// ExternalService is one protected external dependency; *external.Service implements it.
type ExternalService interface {
	Name() string
	Get(ctx context.Context, path, cacheKey string) (external.Response, error)
}

// insights returns the AI service's assessment of a node (ADR-012).
func (s *Server) insights(w http.ResponseWriter, r *http.Request) {
	nodeID, ok := s.nodeIDParam(w, r)
	if !ok {
		return
	}
	resp, err := s.ai.Get(r.Context(), "/v1/insights/"+url.PathEscape(nodeID), nodeID)
	s.writeExternal(w, r, s.ai.Name(), resp, err)
}

// corporate returns the corporate service's status of the node's bunker. The bunker comes from
// the node registry, so an unknown node is a 404 before any external call.
func (s *Server) corporate(w http.ResponseWriter, r *http.Request) {
	nodeID, ok := s.nodeIDParam(w, r)
	if !ok {
		return
	}
	node, err := s.store.QueryNode(r.Context(), nodeID)
	if err != nil {
		s.writeStoreError(w, r, err)
		return
	}
	resp, err := s.corp.Get(r.Context(), "/v1/bunkers/"+url.PathEscape(node.BunkerName)+"/status", node.BunkerName)
	s.writeExternal(w, r, s.corp.Name(), resp, err)
}

func (s *Server) nodeIDParam(w http.ResponseWriter, r *http.Request) (string, bool) {
	nodeID := chi.URLParam(r, "node_id")
	if !eopclient.ValidNodeID(nodeID) {
		s.writeError(w, r, http.StatusBadRequest, errorBody{
			Code:    CodeInvalidNodeID,
			Message: "node_id must be 1 to 63 characters from A-Z, a-z, 0-9, '.', '_' and '-'",
			Field:   "node_id",
		})
		return "", false
	}
	return nodeID, true
}

// writeExternal answers with fresh data, stale cached data marked in meta, or a documented
// degraded response; never a 500 (US-303 AC3, AC6).
func (s *Server) writeExternal(w http.ResponseWriter, r *http.Request, service string, resp external.Response, err error) {
	var degraded *external.DegradedError
	var rejected *external.RejectedError
	switch {
	case err == nil:
		m := s.meta(r)
		if resp.Stale {
			m.Stale = true
			m.StaleSince = resp.StaleSince.UTC().Format(time.RFC3339Nano)
		}
		s.writeJSON(w, r, http.StatusOK, dataEnvelope{Data: resp.Body, Meta: m})
	case errors.As(err, &degraded):
		w.Header().Set("Retry-After", strconv.Itoa(int(math.Ceil(degraded.RetryAfter.Seconds()))))
		s.writeError(w, r, http.StatusServiceUnavailable, errorBody{
			Code: CodeServiceDegraded, Message: "the " + service + " service is temporarily unavailable; retry later",
		})
	case errors.Is(err, external.ErrNotFound):
		s.writeError(w, r, http.StatusNotFound, errorBody{Code: CodeExternalMissing, Message: "the " + service + " service has no data for this node"})
	case errors.As(err, &rejected):
		s.logger.ErrorContext(r.Context(), "external service rejected the gateway's request", "service", service, "error", err)
		s.writeError(w, r, http.StatusBadGateway, errorBody{Code: CodeUpstreamError, Message: "the " + service + " service returned an unexpected answer"})
	default: // the client went away
		s.writeError(w, r, http.StatusServiceUnavailable, errorBody{Code: CodeServiceDegraded, Message: "the request was cancelled"})
	}
}

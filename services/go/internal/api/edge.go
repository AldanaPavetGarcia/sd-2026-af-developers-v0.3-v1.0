package api

import (
	"context"
	"net/http"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
)

// EdgeStatusReader returns the edge ingestor's latest snapshot (ADR-028 §4); ok is false when
// the ingestor wrote none recently. The gateway never calls the ingestor itself.
type EdgeStatusReader interface {
	EdgeStatus(ctx context.Context) (s edge.Snapshot, ok bool, err error)
}

// edgeWindow is the counting window reported when no snapshot exists (protocol §4).
const edgeWindow = time.Minute

// edgeStatus serves GET /v1/edge/status (US-404 AC7): the ingestor's connection state and the
// messages it received and forwarded in the last 60 seconds.
func (s *Server) edgeStatus(w http.ResponseWriter, r *http.Request) {
	if s.edge == nil {
		s.writeData(w, r, http.StatusOK, edge.Disconnected(edgeWindow))
		return
	}
	snapshot, ok, err := s.edge.EdgeStatus(r.Context())
	if err != nil {
		s.logger.WarnContext(r.Context(), "edge status unavailable", "error", err)
		s.writeError(w, r, http.StatusServiceUnavailable, errorBody{Code: CodeUpstreamUnavailable,
			Message: "the edge status store is temporarily unavailable"})
		return
	}
	if !ok {
		snapshot = edge.Disconnected(edgeWindow)
	}
	s.writeData(w, r, http.StatusOK, snapshot)
}

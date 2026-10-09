package api

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/analysis"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/jobs"
)

// Error codes of the graph analysis routes.
const (
	CodeInvalidBody         = "INVALID_BODY"
	CodePayloadTooLarge     = "PAYLOAD_TOO_LARGE"
	CodeJobNotFound         = "JOB_NOT_FOUND"
	CodeInvalidJobID        = "INVALID_JOB_ID"
	CodeInvalidLimit        = "INVALID_LIMIT"
	CodeJobQueueFull        = "JOB_QUEUE_FULL"
	CodeJobStoreUnavailable = "JOB_STORE_UNAVAILABLE"
)

const (
	defaultJobListLimit = 50
	maxJobListLimit     = 200
	queueFullRetryAfter = "5"
	jobsPath            = "/v1/graph/jobs/"
	// hpcEngineNodeID is the node_id an HPC engine registers with; replicas use it as a prefix
	// (ADR-022).
	hpcEngineNodeID = "hpc-engine"
)

// Analyzer runs a synchronous analysis; *eopclient.Client implements it.
type Analyzer interface {
	AnalyzeGraph(ctx context.Context, requestID string, req eopclient.AnalyzeRequest) (json.RawMessage, error)
}

// JobRunner accepts asynchronous jobs; *jobs.Runner implements it.
type JobRunner interface {
	Submit(ctx context.Context, job jobs.Job, req eopclient.AnalyzeRequest) error
}

// JobStore reads jobs; *jobs.Store implements it.
type JobStore interface {
	Get(ctx context.Context, id string) (jobs.Job, error)
	List(ctx context.Context, limit int) ([]jobs.Job, error)
	Ping(ctx context.Context) error
}

// AnalysisOptions configure the graph routes (see config.Analysis).
type AnalysisOptions struct {
	MaxBodyBytes  int
	AsyncMinNodes int
	SyncTimeout   time.Duration
}

type syncResult struct {
	Result json.RawMessage `json:"result"`
}

type acceptedJob struct {
	JobID     string `json:"job_id"`
	Status    string `json:"status"`
	StatusURL string `json:"status_url"`
}

type graphSize struct {
	Nodes int `json:"nodes"`
	Edges int `json:"edges"`
}

type jobView struct {
	JobID       string          `json:"job_id"`
	Status      string          `json:"status"`
	CreatedAt   string          `json:"created_at"`
	StartedAt   string          `json:"started_at,omitempty"`
	CompletedAt string          `json:"completed_at,omitempty"`
	GraphSize   graphSize       `json:"graph_size"`
	Algorithms  []string        `json:"algorithms"`
	Error       *jobs.Failure   `json:"error,omitempty"`
	Result      json.RawMessage `json:"result,omitempty"`
}

func toJobView(j jobs.Job) jobView {
	view := jobView{
		JobID: j.ID, Status: string(j.Status), CreatedAt: timestamp(j.CreatedAt),
		StartedAt: timestamp(j.StartedAt), CompletedAt: timestamp(j.CompletedAt),
		GraphSize: graphSize{Nodes: j.NodeCount, Edges: j.EdgeCount}, Algorithms: j.Algorithms,
		Error: j.Failure, Result: j.Result,
	}
	if view.Algorithms == nil {
		view.Algorithms = []string{}
	}
	return view
}

func timestamp(t time.Time) string {
	if t.IsZero() {
		return ""
	}
	return t.UTC().Format(time.RFC3339Nano)
}

// analyze answers small graphs synchronously (US-302 AC1) and turns large ones into jobs
// (AC2). Invalid input is rejected before anything reaches eop-server (AC7).
func (s *Server) analyze(w http.ResponseWriter, r *http.Request) {
	req, ok := s.decodeAnalyzeRequest(w, r)
	if !ok {
		return
	}
	if verr := analysis.Validate(req); verr != nil {
		s.writeError(w, r, http.StatusBadRequest, errorBody{Code: verr.Code, Message: verr.Message, Field: verr.Field})
		return
	}
	if len(req.Nodes) < s.analysisOpts.AsyncMinNodes {
		s.analyzeSync(w, r, req)
		return
	}
	s.analyzeAsync(w, r, req)
}

func (s *Server) decodeAnalyzeRequest(w http.ResponseWriter, r *http.Request) (eopclient.AnalyzeRequest, bool) {
	var req eopclient.AnalyzeRequest
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, int64(s.analysisOpts.MaxBodyBytes)))
	decoder.DisallowUnknownFields()
	err := decoder.Decode(&req)
	if err == nil && decoder.More() {
		err = errors.New("the body must contain a single JSON object")
	}
	if err == nil {
		return req, true
	}

	var tooLarge *http.MaxBytesError
	var typeErr *json.UnmarshalTypeError
	body := errorBody{Code: CodeInvalidBody, Field: "body"}
	switch {
	case errors.As(err, &tooLarge):
		s.writeError(w, r, http.StatusRequestEntityTooLarge, errorBody{
			Code: CodePayloadTooLarge, Message: "the request body exceeds " + strconv.Itoa(s.analysisOpts.MaxBodyBytes) + " bytes", Field: "body",
		})
		return req, false
	case errors.As(err, &typeErr):
		body.Field = typeErr.Field
		body.Message = "must be of type " + typeErr.Type.String()
	case strings.HasPrefix(err.Error(), "json: unknown field "):
		body.Field = strings.Trim(strings.TrimPrefix(err.Error(), "json: unknown field "), `"`)
		body.Message = "is not a known field"
	case errors.Is(err, io.EOF):
		body.Message = "the request body is empty"
	default:
		body.Message = "the request body is not valid JSON"
	}
	s.writeError(w, r, http.StatusBadRequest, body)
	return req, false
}

func (s *Server) analyzeSync(w http.ResponseWriter, r *http.Request, req eopclient.AnalyzeRequest) {
	ctx, cancel := context.WithTimeout(r.Context(), s.analysisOpts.SyncTimeout)
	defer cancel()
	result, err := s.analyzer.AnalyzeGraph(ctx, s.newID(), req)
	if err != nil {
		outcome := analysis.Classify(err)
		s.logger.WarnContext(r.Context(), "synchronous analysis failed", "code", outcome.Code, "error", err)
		s.writeError(w, r, outcome.Status, errorBody{Code: outcome.Code, Message: outcome.Message, Field: outcome.Field})
		return
	}
	s.writeData(w, r, http.StatusOK, syncResult{Result: result})
}

func (s *Server) analyzeAsync(w http.ResponseWriter, r *http.Request, req eopclient.AnalyzeRequest) {
	// A job accepted while no engine is registered would only fail later; say so now (AC3).
	available, err := s.engineAvailable(r.Context())
	if err != nil {
		s.writeStoreError(w, r, err)
		return
	}
	if !available {
		outcome := analysis.Classify(&eopclient.ProtocolError{Code: eopclient.CodeServiceUnavailable})
		s.writeError(w, r, outcome.Status, errorBody{Code: outcome.Code, Message: outcome.Message})
		return
	}

	job := jobs.Job{
		ID: s.newID(), CreatedAt: s.now(), NodeCount: len(req.Nodes), EdgeCount: len(req.Edges), Algorithms: req.Algorithms,
	}
	switch err := s.jobRunner.Submit(r.Context(), job, req); {
	case errors.Is(err, jobs.ErrQueueFull):
		w.Header().Set("Retry-After", queueFullRetryAfter)
		s.writeError(w, r, http.StatusTooManyRequests, errorBody{Code: CodeJobQueueFull, Message: "too many analyses are queued; retry later"})
		return
	case errors.Is(err, jobs.ErrStopped):
		s.writeError(w, r, http.StatusServiceUnavailable, errorBody{Code: analysis.CodeGatewayShutdown, Message: "the gateway is shutting down; retry"})
		return
	case err != nil:
		s.writeJobStoreError(w, r, err)
		return
	}
	statusURL := jobsPath + job.ID
	w.Header().Set("Location", statusURL)
	s.writeData(w, r, http.StatusAccepted, acceptedJob{JobID: job.ID, Status: string(jobs.StatusQueued), StatusURL: statusURL})
}

// engineAvailable reports whether an HPC engine is registered and ONLINE (ADR-022 naming).
func (s *Server) engineAvailable(ctx context.Context) (bool, error) {
	nodes, err := s.store.ListNodes(ctx)
	if err != nil {
		return false, err
	}
	for _, n := range nodes {
		isEngine := n.NodeID == hpcEngineNodeID || strings.HasPrefix(n.NodeID, hpcEngineNodeID+"-")
		if isEngine && n.Status == "ONLINE" {
			return true, nil
		}
	}
	return false, nil
}

func (s *Server) getJob(w http.ResponseWriter, r *http.Request) {
	id := chi.URLParam(r, "job_id")
	if _, err := uuid.Parse(id); err != nil {
		s.writeError(w, r, http.StatusBadRequest, errorBody{Code: CodeInvalidJobID, Message: "job_id must be a UUID", Field: "job_id"})
		return
	}
	job, err := s.jobStore.Get(r.Context(), id)
	if errors.Is(err, jobs.ErrNotFound) {
		s.writeError(w, r, http.StatusNotFound, errorBody{Code: CodeJobNotFound, Message: "job not found or already expired"})
		return
	}
	if err != nil {
		s.writeJobStoreError(w, r, err)
		return
	}
	s.writeData(w, r, http.StatusOK, toJobView(job))
}

func (s *Server) listJobs(w http.ResponseWriter, r *http.Request) {
	limit := defaultJobListLimit
	if raw := r.URL.Query().Get("limit"); raw != "" {
		n, err := strconv.Atoi(raw)
		if err != nil || n < 1 || n > maxJobListLimit {
			s.writeError(w, r, http.StatusBadRequest, errorBody{
				Code: CodeInvalidLimit, Message: "limit must be an integer from 1 to " + strconv.Itoa(maxJobListLimit), Field: "limit",
			})
			return
		}
		limit = n
	}
	list, err := s.jobStore.List(r.Context(), limit)
	if err != nil {
		s.writeJobStoreError(w, r, err)
		return
	}
	views := make([]jobView, 0, len(list))
	for _, j := range list {
		views = append(views, toJobView(j))
	}
	s.writeData(w, r, http.StatusOK, views)
}

func (s *Server) writeJobStoreError(w http.ResponseWriter, r *http.Request, err error) {
	s.logger.ErrorContext(r.Context(), "job store unavailable", "error", err)
	s.writeError(w, r, http.StatusServiceUnavailable, errorBody{Code: CodeJobStoreUnavailable, Message: "analysis jobs are temporarily unavailable"})
}

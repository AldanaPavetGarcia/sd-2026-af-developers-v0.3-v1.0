// Package analysis validates graph analysis requests and classifies their failures, for both
// the synchronous path and asynchronous jobs (US-302, ADR-010).
package analysis

import (
	"context"
	"errors"
	"fmt"
	"math"
	"net/http"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
)

// SupportedAlgorithms are the algorithm names of the ANALYZE_GRAPH contract.
var SupportedAlgorithms = []string{"shortest_paths", "connected_components", "centrality"}

// Public error codes of graph analysis. They are part of the API contract.
const (
	CodeInvalidGraph         = "INVALID_GRAPH"
	CodeUnsupportedAlgorithm = "UNSUPPORTED_ALGORITHM"
	CodeGraphTooLarge        = "GRAPH_TOO_LARGE"
	CodeHPCUnavailable       = "HPC_UNAVAILABLE"
	CodeAnalysisTimeout      = "ANALYSIS_TIMEOUT"
	CodeAnalysisFailed       = "ANALYSIS_FAILED"
	CodeGatewayShutdown      = "GATEWAY_SHUTDOWN"
)

// ValidationError names the first offending field of a request (US-302 AC7).
type ValidationError struct {
	Code    string
	Field   string
	Message string
}

func (e *ValidationError) Error() string { return e.Field + ": " + e.Message }

func invalid(field, format string, args ...any) *ValidationError {
	return &ValidationError{Code: CodeInvalidGraph, Field: field, Message: fmt.Sprintf(format, args...)}
}

// Validate checks the structure the engine would reject anyway, so a bad request is answered at
// once with the offending field instead of occupying the single engine. Size limits stay with
// the engine, which owns them (EOP_MAX_GRAPH_NODES and related keys).
func Validate(req eopclient.AnalyzeRequest) *ValidationError {
	if len(req.Nodes) == 0 {
		return invalid("nodes", "must contain at least one node")
	}
	seen := make(map[string]struct{}, len(req.Nodes))
	for i, node := range req.Nodes {
		if node == "" {
			return invalid(fmt.Sprintf("nodes[%d]", i), "must not be empty")
		}
		if _, dup := seen[node]; dup {
			return invalid(fmt.Sprintf("nodes[%d]", i), "duplicates node %q", node)
		}
		seen[node] = struct{}{}
	}
	if req.Edges == nil {
		return invalid("edges", "is required; use [] for a graph without edges")
	}
	for i, edge := range req.Edges {
		if _, ok := seen[edge.From]; !ok {
			return invalid(fmt.Sprintf("edges[%d].from", i), "references unknown node %q", edge.From)
		}
		if _, ok := seen[edge.To]; !ok {
			return invalid(fmt.Sprintf("edges[%d].to", i), "references unknown node %q", edge.To)
		}
		if !(edge.Weight > 0) || math.IsInf(edge.Weight, 0) {
			return invalid(fmt.Sprintf("edges[%d].weight", i), "must be a positive, finite number")
		}
	}
	if len(req.Algorithms) == 0 {
		return invalid("algorithms", "must name at least one of %v", SupportedAlgorithms)
	}
	for i, algorithm := range req.Algorithms {
		if !supported(algorithm) {
			return &ValidationError{
				Code:    CodeUnsupportedAlgorithm,
				Field:   fmt.Sprintf("algorithms[%d]", i),
				Message: fmt.Sprintf("%q is not one of %v", algorithm, SupportedAlgorithms),
			}
		}
	}
	return nil
}

func supported(algorithm string) bool {
	for _, s := range SupportedAlgorithms {
		if s == algorithm {
			return true
		}
	}
	return false
}

// Outcome is how a failed analysis is reported: HTTP status for the synchronous path, code and
// message for both paths. Messages never carry internal details (US-302 AC3).
type Outcome struct {
	Status  int
	Code    string
	Message string
	Field   string
}

// Classify maps an analysis error to its public outcome.
func Classify(err error) Outcome {
	var aerr *eopclient.AnalysisError
	switch {
	case eopclient.IsEngineUnavailable(err):
		return Outcome{http.StatusServiceUnavailable, CodeHPCUnavailable, "the graph analysis engine is temporarily unavailable", ""}
	case errors.Is(err, context.Canceled):
		return Outcome{http.StatusServiceUnavailable, CodeGatewayShutdown, "the gateway stopped before the analysis finished; submit it again", ""}
	case errors.Is(err, context.DeadlineExceeded):
		return Outcome{http.StatusGatewayTimeout, CodeAnalysisTimeout, "the analysis did not finish in time", ""}
	case eopclient.IsUnavailable(err):
		return Outcome{http.StatusServiceUnavailable, CodeHPCUnavailable, "the graph analysis service is temporarily unavailable", ""}
	case errors.As(err, &aerr):
		return classifyEngineError(aerr)
	}
	return Outcome{http.StatusBadGateway, CodeAnalysisFailed, "the analysis failed", ""}
}

func classifyEngineError(aerr *eopclient.AnalysisError) Outcome {
	switch aerr.Code {
	case "ERROR_INVALID_GRAPH", "ERROR_INVALID_REQUEST":
		return Outcome{http.StatusBadRequest, CodeInvalidGraph, orDefault(aerr.Message, "the graph is not valid"), aerr.Field}
	case "ERROR_UNSUPPORTED_ALGORITHM":
		return Outcome{http.StatusBadRequest, CodeUnsupportedAlgorithm, orDefault(aerr.Message, "an algorithm is not supported"), aerr.Field}
	case "ERROR_GRAPH_TOO_LARGE":
		return Outcome{http.StatusRequestEntityTooLarge, CodeGraphTooLarge, orDefault(aerr.Message, "the graph exceeds the configured size limits"), aerr.Field}
	}
	// ERROR_ANALYSIS_FAILED and anything unknown: the engine's text may describe internals.
	return Outcome{http.StatusBadGateway, CodeAnalysisFailed, "the analysis failed", ""}
}

// orDefault keeps the engine's validation message, which describes the caller's input, and
// falls back to a generic one when the engine sent none (the contract requires a message).
func orDefault(message, fallback string) string {
	if message == "" {
		return fallback
	}
	return message
}

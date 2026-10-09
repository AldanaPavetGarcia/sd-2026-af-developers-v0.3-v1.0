package analysis

import (
	"context"
	"errors"
	"math"
	"net/http"
	"testing"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
)

func validRequest() eopclient.AnalyzeRequest {
	return eopclient.AnalyzeRequest{
		Nodes:      []string{"A", "B", "C"},
		Edges:      []eopclient.Edge{{From: "A", To: "B", Weight: 1}, {From: "B", To: "C", Weight: 2.5}},
		Algorithms: []string{"connected_components", "centrality"},
	}
}

func TestValidateAcceptsAValidGraph(t *testing.T) {
	if err := Validate(validRequest()); err != nil {
		t.Fatalf("Validate = %v", err)
	}
	noEdges := validRequest()
	noEdges.Edges = []eopclient.Edge{}
	if err := Validate(noEdges); err != nil {
		t.Fatalf("a graph without edges is valid: %v", err)
	}
}

func TestValidateNamesTheOffendingField(t *testing.T) {
	tests := map[string]struct {
		mutate func(*eopclient.AnalyzeRequest)
		field  string
		code   string
	}{
		"no nodes":          {func(r *eopclient.AnalyzeRequest) { r.Nodes = nil }, "nodes", CodeInvalidGraph},
		"empty node":        {func(r *eopclient.AnalyzeRequest) { r.Nodes[1] = "" }, "nodes[1]", CodeInvalidGraph},
		"duplicate node":    {func(r *eopclient.AnalyzeRequest) { r.Nodes[2] = "A" }, "nodes[2]", CodeInvalidGraph},
		"missing edges":     {func(r *eopclient.AnalyzeRequest) { r.Edges = nil }, "edges", CodeInvalidGraph},
		"unknown from":      {func(r *eopclient.AnalyzeRequest) { r.Edges[1].From = "Z" }, "edges[1].from", CodeInvalidGraph},
		"unknown to":        {func(r *eopclient.AnalyzeRequest) { r.Edges[0].To = "Z" }, "edges[0].to", CodeInvalidGraph},
		"zero weight":       {func(r *eopclient.AnalyzeRequest) { r.Edges[0].Weight = 0 }, "edges[0].weight", CodeInvalidGraph},
		"negative weight":   {func(r *eopclient.AnalyzeRequest) { r.Edges[1].Weight = -3 }, "edges[1].weight", CodeInvalidGraph},
		"infinite weight":   {func(r *eopclient.AnalyzeRequest) { r.Edges[0].Weight = math.Inf(1) }, "edges[0].weight", CodeInvalidGraph},
		"NaN weight":        {func(r *eopclient.AnalyzeRequest) { r.Edges[0].Weight = math.NaN() }, "edges[0].weight", CodeInvalidGraph},
		"no algorithms":     {func(r *eopclient.AnalyzeRequest) { r.Algorithms = nil }, "algorithms", CodeInvalidGraph},
		"unknown algorithm": {func(r *eopclient.AnalyzeRequest) { r.Algorithms[1] = "pagerank" }, "algorithms[1]", CodeUnsupportedAlgorithm},
	}
	for name, tt := range tests {
		t.Run(name, func(t *testing.T) {
			req := validRequest()
			tt.mutate(&req)
			err := Validate(req)
			if err == nil || err.Field != tt.field || err.Code != tt.code || err.Error() == "" {
				t.Fatalf("Validate = %+v, want field %s code %s", err, tt.field, tt.code)
			}
		})
	}
}

func TestClassify(t *testing.T) {
	tests := map[string]struct {
		err    error
		status int
		code   string
	}{
		"no engine (text)":   {&eopclient.ProtocolError{Code: eopclient.CodeServiceUnavailable}, http.StatusServiceUnavailable, CodeHPCUnavailable},
		"engine timeout":     {&eopclient.ProtocolError{Code: eopclient.CodeHPCTimeout}, http.StatusServiceUnavailable, CodeHPCUnavailable},
		"no engine (json)":   {&eopclient.AnalysisError{Code: eopclient.CodeServiceUnavailable}, http.StatusServiceUnavailable, CodeHPCUnavailable},
		"server unreachable": {&eopclient.UnavailableError{Err: errors.New("refused")}, http.StatusServiceUnavailable, CodeHPCUnavailable},
		"gateway deadline":   {context.DeadlineExceeded, http.StatusGatewayTimeout, CodeAnalysisTimeout},
		"shutdown":           {context.Canceled, http.StatusServiceUnavailable, CodeGatewayShutdown},
		"invalid graph":      {&eopclient.AnalysisError{Code: "ERROR_INVALID_GRAPH", Field: "payload.edges[0]"}, http.StatusBadRequest, CodeInvalidGraph},
		"invalid request":    {&eopclient.AnalysisError{Code: "ERROR_INVALID_REQUEST"}, http.StatusBadRequest, CodeInvalidGraph},
		"unsupported":        {&eopclient.AnalysisError{Code: "ERROR_UNSUPPORTED_ALGORITHM"}, http.StatusBadRequest, CodeUnsupportedAlgorithm},
		"too large":          {&eopclient.AnalysisError{Code: "ERROR_GRAPH_TOO_LARGE"}, http.StatusRequestEntityTooLarge, CodeGraphTooLarge},
		"analysis failed":    {&eopclient.AnalysisError{Code: "ERROR_ANALYSIS_FAILED", Message: "std::bad_alloc at engine.cpp:88"}, http.StatusBadGateway, CodeAnalysisFailed},
		"malformed answer":   {&eopclient.MalformedResponseError{Line: "x"}, http.StatusBadGateway, CodeAnalysisFailed},
	}
	for name, tt := range tests {
		t.Run(name, func(t *testing.T) {
			got := Classify(tt.err)
			if got.Status != tt.status || got.Code != tt.code || got.Message == "" {
				t.Fatalf("Classify = %+v, want %d %s", got, tt.status, tt.code)
			}
		})
	}
	if got := Classify(&eopclient.AnalysisError{Code: "ERROR_ANALYSIS_FAILED", Message: "std::bad_alloc at engine.cpp:88"}); got.Message != "the analysis failed" {
		t.Fatalf("internal engine details leaked: %q", got.Message)
	}
	if got := Classify(&eopclient.AnalysisError{Code: "ERROR_INVALID_GRAPH", Field: "payload.nodes[2]"}); got.Field != "payload.nodes[2]" {
		t.Fatalf("engine validation field not kept: %+v", got)
	}
}

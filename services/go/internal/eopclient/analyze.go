package eopclient

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

// Codes the server or the engine use when the analysis could not run at all. They are the
// "HPC engine is offline" case of US-302 AC3.
const (
	CodeServiceUnavailable = "ERROR_SERVICE_UNAVAILABLE"
	CodeHPCTimeout         = "ERROR_HPC_TIMEOUT"
)

const (
	analyzeType     = "ANALYZE_GRAPH"
	responseType    = "RESPONSE"
	responseAck     = "ACK"
	responseError   = "ERROR"
	jsonObjectStart = '{'
)

// Edge is a weighted, directed edge of an analysis request.
type Edge struct {
	From   string  `json:"from"`
	To     string  `json:"to"`
	Weight float64 `json:"weight"`
}

// AnalyzeRequest is the payload of ANALYZE_GRAPH (docs/analyze-graph-protocol-v0.2.md).
type AnalyzeRequest struct {
	Nodes      []string `json:"nodes"`
	Edges      []Edge   `json:"edges"`
	Algorithms []string `json:"algorithms"`
}

// AnalysisError is a structured ERROR answer from the HPC engine.
type AnalysisError struct {
	Code      string `json:"error_code"`
	Message   string `json:"message"`
	Field     string `json:"field"`
	Retryable bool   `json:"retryable"`
}

func (e *AnalysisError) Error() string {
	return fmt.Sprintf("eopclient: analysis failed with %s: %s", e.Code, e.Message)
}

type analyzeEnvelope struct {
	Type        string         `json:"type"`
	RequestID   string         `json:"request_id"`
	Traceparent string         `json:"traceparent,omitempty"`
	Payload     AnalyzeRequest `json:"payload"`
}

type analyzeResponse struct {
	Type      string          `json:"type"`
	Status    string          `json:"status"`
	RequestID string          `json:"request_id"`
	Result    json.RawMessage `json:"result"`
	AnalysisError
}

// AnalyzeGraph sends one ANALYZE_GRAPH request and returns the engine's result object as raw
// JSON. It is never retried: an analysis is expensive and the engine is a single, serialized
// resource (ADR-010).
func (c *Client) AnalyzeGraph(ctx context.Context, requestID string, req AnalyzeRequest) (json.RawMessage, error) {
	if requestID == "" || strings.ContainsAny(requestID, "\r\n") {
		return nil, fmt.Errorf("%w: request_id %q", ErrInvalidArgument, requestID)
	}
	envelope := analyzeEnvelope{Type: analyzeType, RequestID: requestID, Traceparent: traceparent(ctx), Payload: req}
	line, err := json.Marshal(envelope) // json.Marshal escapes newlines, so the message is one line
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInvalidArgument, err)
	}

	var result json.RawMessage
	// Never retried, and the connection is not reused: answers are large and rare, and closing it
	// leaves nothing behind if the server's framing ever produced an extra line.
	err = c.exchangeOnPool(ctx, string(line)+"\n", c.opts.AnalyzeTimeout, false, false, func(pc *conn) error {
		answer, err := readLongLine(pc.reader, c.opts.MaxAnalyzeResponseBytes)
		if err != nil {
			return err
		}
		if perr := parseErrorLine(answer); perr != nil {
			return perr // server-side text errors: no engine registered, engine timed out
		}
		result, err = parseAnalyzeResponse(answer, requestID)
		return err
	})
	return result, err
}

func parseAnalyzeResponse(answer, requestID string) (json.RawMessage, error) {
	if answer == "" || answer[0] != jsonObjectStart {
		return nil, &MalformedResponseError{Line: truncate(answer), Reason: "expected a JSON RESPONSE"}
	}
	var resp analyzeResponse
	if err := json.Unmarshal([]byte(answer), &resp); err != nil {
		return nil, &MalformedResponseError{Line: truncate(answer), Reason: "invalid JSON: " + err.Error()}
	}
	if resp.Type != responseType {
		return nil, &MalformedResponseError{Line: truncate(answer), Reason: "type is not RESPONSE"}
	}
	if resp.RequestID != "" && resp.RequestID != requestID {
		return nil, &MalformedResponseError{Line: truncate(answer), Reason: "answer belongs to another request_id"}
	}
	switch resp.Status {
	case responseAck:
		if len(resp.Result) == 0 || resp.Result[0] != jsonObjectStart {
			return nil, &MalformedResponseError{Line: truncate(answer), Reason: "ACK without a result object"}
		}
		return resp.Result, nil
	case responseError:
		analysisErr := resp.AnalysisError
		if analysisErr.Code == "" {
			return nil, &MalformedResponseError{Line: truncate(answer), Reason: "ERROR without error_code"}
		}
		return nil, &analysisErr
	default:
		return nil, &MalformedResponseError{Line: truncate(answer), Reason: "unknown status " + resp.Status}
	}
}

// readLongLine reads a line that may be far larger than the reader's buffer, up to max bytes.
func readLongLine(reader *bufio.Reader, maxBytes int) (string, error) {
	var buf []byte
	for {
		chunk, err := reader.ReadSlice('\n')
		if len(buf)+len(chunk) > maxBytes {
			return "", &MalformedResponseError{Line: truncate(string(append(buf, chunk...))), Reason: "answer exceeds the configured limit"}
		}
		buf = append(buf, chunk...)
		if errors.Is(err, bufio.ErrBufferFull) {
			continue
		}
		if err != nil {
			return "", err
		}
		return strings.TrimRight(string(buf), "\r\n"), nil
	}
}

// IsEngineUnavailable reports whether err means the analysis could not run because no engine
// was available or it did not answer in time (US-302 AC3).
func IsEngineUnavailable(err error) bool {
	var perr *ProtocolError
	if errors.As(err, &perr) {
		return perr.Code == CodeServiceUnavailable || perr.Code == CodeHPCTimeout
	}
	var aerr *AnalysisError
	if errors.As(err, &aerr) {
		return aerr.Code == CodeServiceUnavailable || aerr.Code == CodeHPCTimeout
	}
	return false
}

func truncate(s string) string {
	const maxShown = 120
	if len(s) > maxShown {
		return s[:maxShown] + "…"
	}
	return s
}

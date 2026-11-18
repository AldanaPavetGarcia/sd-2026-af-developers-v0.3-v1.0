package mttr

import (
	"bufio"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Where a Pod finds its service account (Kubernetes in-cluster configuration).
const serviceAccountDir = "/var/run/secrets/kubernetes.io/serviceaccount"

// maxWatchLine bounds one watch event; an EndpointSlice or Pod is a few KB.
const maxWatchLine = 1 << 20

// watchRetryDelay separates a broken watch from the next list.
var watchRetryDelay = time.Second

// Kube reads Pods and EndpointSlices from the Kubernetes API with list + watch (ADR-027 §4:
// namespaced, `list`/`watch` only). It uses the API over HTTP directly, without client-go.
type Kube struct {
	BaseURL   string // https://kubernetes.default.svc:443
	Namespace string
	Token     string
	Client    *http.Client
	Logger    *slog.Logger // nil: errors are not logged
}

// InCluster builds a Kube from the Pod's service account.
func InCluster() (*Kube, error) {
	return inClusterFrom(serviceAccountDir, os.Getenv("KUBERNETES_SERVICE_HOST"), os.Getenv("KUBERNETES_SERVICE_PORT"))
}

func inClusterFrom(dir, host, port string) (*Kube, error) {
	if host == "" || port == "" {
		return nil, errors.New("not running in a cluster: KUBERNETES_SERVICE_HOST/PORT are unset")
	}
	token, err := os.ReadFile(filepath.Join(dir, "token"))
	if err != nil {
		return nil, fmt.Errorf("service account token: %w", err)
	}
	namespace, err := os.ReadFile(filepath.Join(dir, "namespace"))
	if err != nil {
		return nil, fmt.Errorf("service account namespace: %w", err)
	}
	ca, err := os.ReadFile(filepath.Join(dir, "ca.crt"))
	if err != nil {
		return nil, fmt.Errorf("service account CA: %w", err)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(ca) {
		return nil, errors.New("service account CA: no certificate")
	}
	return &Kube{
		BaseURL:   "https://" + host + ":" + port,
		Namespace: strings.TrimSpace(string(namespace)),
		Token:     strings.TrimSpace(string(token)),
		Client:    &http.Client{Transport: &http.Transport{TLSClientConfig: &tls.Config{RootCAs: pool, MinVersion: tls.VersionTLS12}}},
	}, nil
}

// objectMeta is the part of metadata both kinds need.
type objectMeta struct {
	Name              string  `json:"name"`
	ResourceVersion   string  `json:"resourceVersion"`
	DeletionTimestamp *string `json:"deletionTimestamp"`
}

type pod struct {
	Metadata objectMeta `json:"metadata"`
	Status   struct {
		Conditions []struct {
			Type   string `json:"type"`
			Status string `json:"status"`
		} `json:"conditions"`
	} `json:"status"`
}

type endpointSlice struct {
	Metadata  objectMeta `json:"metadata"`
	Endpoints []struct {
		Conditions struct {
			Ready *bool `json:"ready"`
		} `json:"conditions"`
		TargetRef *struct {
			Kind string `json:"kind"`
			Name string `json:"name"`
		} `json:"targetRef"`
	} `json:"endpoints"`
}

type listResponse struct {
	Metadata struct {
		ResourceVersion string `json:"resourceVersion"`
	} `json:"metadata"`
	Items []json.RawMessage `json:"items"`
}

type watchEvent struct {
	Type   string          `json:"type"`
	Object json.RawMessage `json:"object"`
}

// Event is one object change: ADDED, MODIFIED, DELETED, as the API names them.
type Event struct {
	Type   string
	Object json.RawMessage
}

// podState reads a Pod object.
func podState(raw json.RawMessage, gone bool) (PodState, error) {
	var p pod
	if err := json.Unmarshal(raw, &p); err != nil {
		return PodState{}, err
	}
	state := PodState{Name: p.Metadata.Name, Deleting: p.Metadata.DeletionTimestamp != nil, Gone: gone}
	for _, c := range p.Status.Conditions {
		if c.Type == "Ready" {
			state.Ready = c.Status == "True"
		}
	}
	return state, nil
}

// sliceRoutable reads which Pods an EndpointSlice routes to. An endpoint without a ready
// condition counts as ready, as the API defines it.
func sliceRoutable(raw json.RawMessage) (string, map[string]bool, error) {
	var s endpointSlice
	if err := json.Unmarshal(raw, &s); err != nil {
		return "", nil, err
	}
	routable := map[string]bool{}
	for _, e := range s.Endpoints {
		if e.TargetRef == nil || e.TargetRef.Kind != "Pod" {
			continue
		}
		routable[e.TargetRef.Name] = e.Conditions.Ready == nil || *e.Conditions.Ready
	}
	return s.Metadata.Name, routable, nil
}

// Watch lists the collection at `path` (filtered by `selector`), sends every item as ADDED, then
// watches it until ctx ends, listing again whenever the watch breaks.
func (k *Kube) Watch(ctx context.Context, path, selector string, events chan<- Event) error {
	for {
		version, err := k.list(ctx, path, selector, events)
		if err == nil {
			err = k.watch(ctx, path, selector, version, events)
		}
		if ctx.Err() != nil {
			return ctx.Err()
		}
		// A broken watch or an expired resourceVersion: start over shortly.
		if k.Logger != nil && !errors.Is(err, io.EOF) {
			k.Logger.Warn("kubernetes watch interrupted; listing again", "path", path, "error", err)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(watchRetryDelay):
		}
	}
}

func (k *Kube) request(ctx context.Context, path string, query url.Values) (*http.Response, error) {
	u := k.BaseURL + path + "?" + query.Encode()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+k.Token)
	req.Header.Set("Accept", "application/json")
	resp, err := k.Client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
		_ = resp.Body.Close()
		return nil, fmt.Errorf("GET %s: %s: %s", path, resp.Status, strings.TrimSpace(string(body)))
	}
	return resp, nil
}

func (k *Kube) list(ctx context.Context, path, selector string, events chan<- Event) (string, error) {
	resp, err := k.request(ctx, path, url.Values{"labelSelector": {selector}})
	if err != nil {
		return "", err
	}
	defer func() { _ = resp.Body.Close() }()
	var list listResponse
	if err := json.NewDecoder(resp.Body).Decode(&list); err != nil {
		return "", err
	}
	for _, item := range list.Items {
		select {
		case events <- Event{Type: "ADDED", Object: item}:
		case <-ctx.Done():
			return "", ctx.Err()
		}
	}
	return list.Metadata.ResourceVersion, nil
}

func (k *Kube) watch(ctx context.Context, path, selector, version string, events chan<- Event) error {
	resp, err := k.request(ctx, path, url.Values{"labelSelector": {selector}, "watch": {"true"}, "resourceVersion": {version}})
	if err != nil {
		return err
	}
	defer func() { _ = resp.Body.Close() }()
	scanner := bufio.NewScanner(resp.Body)
	scanner.Buffer(make([]byte, 0, 64*1024), maxWatchLine)
	for scanner.Scan() {
		var ev watchEvent
		if err := json.Unmarshal(scanner.Bytes(), &ev); err != nil {
			return err
		}
		if ev.Type == "ERROR" {
			return fmt.Errorf("watch %s: %s", path, string(ev.Object))
		}
		if ev.Type == "BOOKMARK" {
			continue
		}
		select {
		case events <- Event(ev):
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	if err := scanner.Err(); err != nil {
		return err
	}
	return io.EOF
}

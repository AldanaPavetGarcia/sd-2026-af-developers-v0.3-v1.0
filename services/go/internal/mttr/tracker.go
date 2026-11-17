// Package mttr measures how long eop-server is out of service when one of its Pods fails
// (ADR-027 §4, US-407 AC4): from Kubernetes events, the time until the Service no longer routes to
// the failed Pod while another replica is ready (service restoration, the MTTR checked against
// 200 ms), and the time until a replacement Pod is Ready (replica replacement, the PRD's literal
// "pod death → pod ready", shown next to it). A synthetic client probe (probe.go) gives the
// client-side view of the same outage.
package mttr

import (
	"sort"
	"time"
)

// Kind tells the two durations apart.
type Kind string

const (
	// ServiceRestoration: failure → the Service routes only to ready replicas again.
	ServiceRestoration Kind = "service_restoration"
	// ReplicaReplacement: failure → a replacement (or the restarted Pod) is Ready.
	ReplicaReplacement Kind = "replica_replacement"
)

// Measurement is one duration of one failure.
type Measurement struct {
	Kind     Kind
	Pod      string
	Duration time.Duration
}

// PodState is what the tracker needs from a Pod event.
type PodState struct {
	Name     string
	Ready    bool // condition Ready is True
	Deleting bool // metadata.deletionTimestamp is set
	Gone     bool // the Pod object was deleted
}

// A failure whose replacement never comes (a rollout removes old Pods after their replacements
// are already Ready) is forgotten after this long.
const failureExpiry = 5 * time.Minute

type failure struct {
	pod       string
	at        time.Time
	readyPods map[string]bool // Pods already Ready at the failure: they do not replace it
	restored  bool
	replaced  bool
}

// Tracker turns Pod and EndpointSlice events, stamped on arrival, into measurements. It is not
// safe for concurrent use; the watcher feeds it from one goroutine.
type Tracker struct {
	pods     map[string]PodState
	routable map[string]bool // Pod name → ready endpoint in the Service
	failures []*failure
	Failures int // failures seen since start
}

// NewTracker returns an empty tracker.
func NewTracker() *Tracker {
	return &Tracker{pods: map[string]PodState{}, routable: map[string]bool{}}
}

// OnPod records a Pod event received at `at`.
func (t *Tracker) OnPod(p PodState, at time.Time) []Measurement {
	prev, known := t.pods[p.Name]
	if p.Gone {
		delete(t.pods, p.Name)
	} else {
		t.pods[p.Name] = p
	}

	var out []Measurement
	failed := known && !prev.Deleting && (p.Deleting || p.Gone || (prev.Ready && !p.Ready))
	if failed && t.open(p.Name) == nil {
		f := &failure{pod: p.Name, at: at, readyPods: map[string]bool{}}
		for name, state := range t.pods {
			if state.Ready && name != p.Name {
				f.readyPods[name] = true
			}
		}
		t.failures = append(t.failures, f)
		t.Failures++
		// The Service may already have dropped the Pod before this event arrived.
		out = append(out, t.checkRestored(at)...)
	}

	if p.Ready && !p.Deleting && !p.Gone && (!known || !prev.Ready) {
		out = append(out, t.replacedBy(p.Name, at)...)
	}
	t.forgetFinished(at)
	return out
}

// OnEndpoints records the routable Pods of the Service (every ready endpoint of every
// EndpointSlice), received at `at`.
func (t *Tracker) OnEndpoints(routable map[string]bool, at time.Time) []Measurement {
	t.routable = routable
	out := t.checkRestored(at)
	t.forgetFinished(at)
	return out
}

func (t *Tracker) open(pod string) *failure {
	for _, f := range t.failures {
		if f.pod == pod {
			return f
		}
	}
	return nil
}

func (t *Tracker) checkRestored(at time.Time) []Measurement {
	var out []Measurement
	for _, f := range t.failures {
		if f.restored || t.routable[f.pod] || !t.othersRoutable(f.pod) {
			continue
		}
		f.restored = true
		out = append(out, Measurement{Kind: ServiceRestoration, Pod: f.pod, Duration: nonNegative(at.Sub(f.at))})
	}
	return out
}

func (t *Tracker) othersRoutable(pod string) bool {
	for name, ready := range t.routable {
		if ready && name != pod {
			return true
		}
	}
	return false
}

// A Pod that becomes Ready replaces the most recent failure it can: a Pod that was not Ready at
// the failure replaces it, the failed Pod itself (restarted in place) only its own. The most
// recent one, because an older open failure is usually a rollout's, already replaced by a surge
// Pod before it was removed.
func (t *Tracker) replacedBy(pod string, at time.Time) []Measurement {
	for i := len(t.failures) - 1; i >= 0; i-- {
		f := t.failures[i]
		if f.replaced {
			continue
		}
		if f.pod == pod || !f.readyPods[pod] {
			f.replaced = true
			return []Measurement{{Kind: ReplicaReplacement, Pod: f.pod, Duration: nonNegative(at.Sub(f.at))}}
		}
	}
	return nil
}

func (t *Tracker) forgetFinished(now time.Time) {
	kept := t.failures[:0]
	for _, f := range t.failures {
		if (!f.restored || !f.replaced) && now.Sub(f.at) < failureExpiry {
			kept = append(kept, f)
		}
	}
	t.failures = kept
}

// ReadyReplicas is how many Pods are routable now.
func (t *Tracker) ReadyReplicas() int {
	n := 0
	for _, ready := range t.routable {
		if ready {
			n++
		}
	}
	return n
}

// Open lists the Pods whose failure is not fully measured yet, sorted.
func (t *Tracker) Open() []string {
	names := make([]string, 0, len(t.failures))
	for _, f := range t.failures {
		names = append(names, f.pod)
	}
	sort.Strings(names)
	return names
}

func nonNegative(d time.Duration) time.Duration {
	if d < 0 {
		return 0
	}
	return d
}

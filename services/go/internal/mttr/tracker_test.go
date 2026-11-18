package mttr

import (
	"reflect"
	"testing"
	"time"
)

var t0 = time.Date(2026, 11, 20, 12, 0, 0, 0, time.UTC)

func ms(n int) time.Time { return t0.Add(time.Duration(n) * time.Millisecond) }

func ready(names ...string) map[string]bool {
	m := map[string]bool{}
	for _, n := range names {
		m[n] = true
	}
	return m
}

// Two Ready replicas, both routable, as after a rollout.
func steady() *Tracker {
	tr := NewTracker()
	tr.OnPod(PodState{Name: "a", Ready: true}, t0)
	tr.OnPod(PodState{Name: "b", Ready: true}, t0)
	tr.OnEndpoints(ready("a", "b"), t0)
	return tr
}

func only(t *testing.T, got []Measurement, kind Kind, pod string, d time.Duration) {
	t.Helper()
	want := []Measurement{{Kind: kind, Pod: pod, Duration: d}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %+v, want %+v", got, want)
	}
}

// kubectl delete pod: deletion → the Service drops the Pod → a replacement is Ready.
func TestDeletedPodIsMeasuredTwice(t *testing.T) {
	tr := steady()
	if got := tr.OnPod(PodState{Name: "a", Ready: true, Deleting: true}, ms(0)); got != nil {
		t.Fatalf("nothing is measured yet: %+v", got)
	}
	if tr.Failures != 1 {
		t.Fatalf("failures = %d", tr.Failures)
	}
	only(t, tr.OnEndpoints(ready("b"), ms(35)), ServiceRestoration, "a", 35*time.Millisecond)
	if tr.ReadyReplicas() != 1 {
		t.Fatalf("ready replicas = %d", tr.ReadyReplicas())
	}

	tr.OnPod(PodState{Name: "c"}, ms(100)) // created, not ready yet
	only(t, tr.OnPod(PodState{Name: "c", Ready: true}, ms(2400)), ReplicaReplacement, "a", 2400*time.Millisecond)
	if open := tr.Open(); len(open) != 0 {
		t.Fatalf("open failures: %v", open)
	}

	// Later events of the deleted Pod change nothing.
	if got := tr.OnPod(PodState{Name: "a", Deleting: true, Gone: true}, ms(3000)); got != nil {
		t.Fatalf("got %+v", got)
	}
	if tr.Failures != 1 {
		t.Fatalf("failures = %d", tr.Failures)
	}
}

// The EndpointSlice update can arrive before the Pod event: restoration is then 0.
func TestServiceAlreadyRestoredWhenTheFailureArrives(t *testing.T) {
	tr := steady()
	tr.OnEndpoints(ready("b"), ms(10))
	only(t, tr.OnPod(PodState{Name: "a", Deleting: true}, ms(30)), ServiceRestoration, "a", 0)
}

// A hard crash: the Pod stays but is no longer Ready, then its container restarts in place.
func TestCrashedPodRestartedInPlace(t *testing.T) {
	tr := steady()
	tr.OnPod(PodState{Name: "a", Ready: false}, ms(0))
	only(t, tr.OnEndpoints(ready("b"), ms(1200)), ServiceRestoration, "a", 1200*time.Millisecond)
	only(t, tr.OnPod(PodState{Name: "a", Ready: true}, ms(5000)), ReplicaReplacement, "a", 5*time.Second)
}

// With no other routable replica, the service is not restored by dropping the failed Pod.
func TestNotRestoredWithoutAnotherReplica(t *testing.T) {
	tr := NewTracker()
	tr.OnPod(PodState{Name: "a", Ready: true}, t0)
	tr.OnEndpoints(ready("a"), t0)
	tr.OnPod(PodState{Name: "a", Deleting: true}, ms(0))
	if got := tr.OnEndpoints(map[string]bool{}, ms(20)); got != nil {
		t.Fatalf("no replica serves: %+v", got)
	}
	tr.OnPod(PodState{Name: "c"}, ms(100))
	only(t, tr.OnPod(PodState{Name: "c", Ready: true}, ms(2000)), ReplicaReplacement, "a", 2*time.Second)
	// The new replica becomes routable: that is when the service came back.
	only(t, tr.OnEndpoints(ready("c"), ms(2050)), ServiceRestoration, "a", 2050*time.Millisecond)
}

// Two replicas fail one after the other: each failure is measured on its own.
func TestTwoFailures(t *testing.T) {
	tr := steady()
	tr.OnPod(PodState{Name: "c", Ready: true}, t0)
	tr.OnEndpoints(ready("a", "b", "c"), t0)

	tr.OnPod(PodState{Name: "a", Deleting: true}, ms(0))
	tr.OnPod(PodState{Name: "b", Deleting: true}, ms(10))
	got := tr.OnEndpoints(ready("c"), ms(50))
	if len(got) != 2 || got[0].Pod != "a" || got[1].Pod != "b" || got[1].Duration != 40*time.Millisecond {
		t.Fatalf("got %+v", got)
	}
	if open := tr.Open(); !reflect.DeepEqual(open, []string{"a", "b"}) {
		t.Fatalf("open = %v", open)
	}
	// The most recent failure gets the first replacement.
	only(t, tr.OnPod(PodState{Name: "d", Ready: true}, ms(3000)), ReplicaReplacement, "b", 2990*time.Millisecond)
	only(t, tr.OnPod(PodState{Name: "e", Ready: true}, ms(3100)), ReplicaReplacement, "a", 3100*time.Millisecond)
}

// A rollout removes an old Pod after its surge replacement is Ready: that failure gets no
// replacement, must not take the next one, and is forgotten after failureExpiry.
func TestARolloutRemovalDoesNotStealTheNextReplacement(t *testing.T) {
	tr := steady()
	tr.OnPod(PodState{Name: "s"}, ms(0))
	tr.OnPod(PodState{Name: "s", Ready: true}, ms(500)) // surge Pod, Ready before the removal
	tr.OnEndpoints(ready("a", "b", "s"), ms(500))
	tr.OnPod(PodState{Name: "a", Deleting: true}, ms(1000)) // the rollout removes "a"
	tr.OnEndpoints(ready("b", "s"), ms(1020))

	// A real failure later, and its replacement.
	tr.OnPod(PodState{Name: "b", Deleting: true}, ms(60_000))
	tr.OnEndpoints(ready("s"), ms(60_040))
	tr.OnPod(PodState{Name: "c"}, ms(60_050))
	only(t, tr.OnPod(PodState{Name: "c", Ready: true}, ms(65_000)), ReplicaReplacement, "b", 5*time.Second)

	if open := tr.Open(); !reflect.DeepEqual(open, []string{"a"}) {
		t.Fatalf("open = %v", open)
	}
	tr.OnEndpoints(ready("s", "c"), t0.Add(failureExpiry+2*time.Second))
	if open := tr.Open(); len(open) != 0 {
		t.Fatalf("the rollout's removal should be forgotten: %v", open)
	}
}

// A replacement created before the failure event arrives (not Ready yet) still counts.
func TestAReplacementSeenBeforeTheFailure(t *testing.T) {
	tr := steady()
	tr.OnPod(PodState{Name: "c"}, ms(0))
	tr.OnPod(PodState{Name: "a", Deleting: true}, ms(5))
	tr.OnEndpoints(ready("b"), ms(40))
	only(t, tr.OnPod(PodState{Name: "c", Ready: true}, ms(3005)), ReplicaReplacement, "a", 3*time.Second)
}

// Pods that were never seen, and Pods starting up, are not failures.
func TestStartupIsNotAFailure(t *testing.T) {
	tr := NewTracker()
	if got := tr.OnPod(PodState{Name: "x", Deleting: true}, ms(0)); got != nil {
		t.Fatalf("unknown pod: %+v", got)
	}
	tr.OnPod(PodState{Name: "y"}, ms(0))
	tr.OnPod(PodState{Name: "y", Ready: true}, ms(10))
	tr.OnPod(PodState{Name: "y", Ready: true}, ms(20)) // repeated MODIFIED
	if tr.Failures != 0 {
		t.Fatalf("failures = %d", tr.Failures)
	}
}

func TestNonNegative(t *testing.T) {
	if nonNegative(-time.Second) != 0 || nonNegative(time.Second) != time.Second {
		t.Fatal("nonNegative")
	}
}

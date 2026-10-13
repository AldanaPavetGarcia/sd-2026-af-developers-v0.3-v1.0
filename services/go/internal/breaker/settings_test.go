package breaker

import (
	"strings"
	"testing"
	"time"
)

var services = []string{"ai", "corporate"}

func TestParseSettingsDefaultsAndOverrides(t *testing.T) {
	settings, err := ParseSettings([]byte(`
defaults:
  min_requests: 20
  call_timeout: 1500ms
services:
  ai:
    reset_timeout: 15s
    failure_threshold: 3
`), services)
	if err != nil {
		t.Fatal(err)
	}
	ai, corp := settings.For("ai"), settings.For("corporate")
	if ai.Breaker.ResetTimeout != 15*time.Second || ai.Breaker.FailureThreshold != 3 || ai.Breaker.MinRequests != 20 || ai.CallTimeout != 1500*time.Millisecond {
		t.Fatalf("ai = %+v", ai)
	}
	if corp.Breaker.ResetTimeout != 30*time.Second || corp.Breaker.MinRequests != 20 || corp.Breaker.ErrorRateThreshold != 0.5 {
		t.Fatalf("corporate = %+v (defaults section must apply)", corp)
	}
	if settings.For("other") != Defaults() {
		t.Fatal("an unlisted service uses Defaults")
	}
}

func TestParseSettingsEmptyFileUsesPRDDefaults(t *testing.T) {
	settings, err := ParseSettings([]byte(""), services)
	if err != nil {
		t.Fatal(err)
	}
	if settings.For("ai") != Defaults() {
		t.Fatalf("ai = %+v", settings.For("ai"))
	}
	d := Defaults().Breaker
	if d.FailureThreshold != 5 || d.ErrorRateThreshold != 0.5 || d.Window != 10*time.Second || d.ResetTimeout != 30*time.Second || d.HalfOpenMaxCalls != 1 {
		t.Fatalf("defaults drifted from ADR-012: %+v", d)
	}
}

func TestParseSettingsRejectsBadFiles(t *testing.T) {
	tests := map[string]string{
		"unknown key":     "defaults:\n  failure_treshold: 3\n",
		"unknown service": "services:\n  billing:\n    min_requests: 3\n",
		"bad duration":    "defaults:\n  window: ten seconds\n",
		"invalid value":   "services:\n  ai:\n    error_rate_threshold: 2\n",
		"zero timeout":    "defaults:\n  call_timeout: 0s\n",
		"not yaml":        "defaults: [\n",
	}
	for name, data := range tests {
		t.Run(name, func(t *testing.T) {
			if _, err := ParseSettings([]byte(data), services); err == nil || !strings.Contains(err.Error(), "breaker settings") {
				t.Fatalf("error = %v", err)
			}
		})
	}
}

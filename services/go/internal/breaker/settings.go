package breaker

import (
	"errors"
	"fmt"
	"time"

	"sigs.k8s.io/yaml"
)

// ServiceSettings are the breaker thresholds of one external service plus its call timeout.
type ServiceSettings struct {
	Breaker     Config
	CallTimeout time.Duration
}

// Defaults of ADR-012: the PRD values plus this ADR's min_requests and call_timeout.
func Defaults() ServiceSettings {
	return ServiceSettings{
		Breaker: Config{
			FailureThreshold: 5, ErrorRateThreshold: 0.5, MinRequests: 10,
			Window: 10 * time.Second, ResetTimeout: 30 * time.Second, HalfOpenMaxCalls: 1,
		},
		CallTimeout: 2 * time.Second,
	}
}

// Settings maps service names to their settings; services not listed use Defaults.
type Settings map[string]ServiceSettings

// For returns the settings of one service.
func (s Settings) For(service string) ServiceSettings {
	if v, ok := s[service]; ok {
		return v
	}
	return Defaults()
}

// fileSettings mirrors the ConfigMap file. Every field is optional; a missing one inherits from
// defaults, and defaults inherit from Defaults().
//
//	defaults:
//	  failure_threshold: 5
//	  error_rate_threshold: 0.5
//	  min_requests: 10
//	  window: 10s
//	  reset_timeout: 30s
//	  half_open_max_calls: 1
//	  call_timeout: 2s
//	services:
//	  ai:
//	    reset_timeout: 15s
type fileSettings struct {
	Defaults fileService            `json:"defaults"`
	Services map[string]fileService `json:"services"`
}

type fileService struct {
	FailureThreshold   *int     `json:"failure_threshold"`
	ErrorRateThreshold *float64 `json:"error_rate_threshold"`
	MinRequests        *int     `json:"min_requests"`
	Window             *string  `json:"window"`
	ResetTimeout       *string  `json:"reset_timeout"`
	HalfOpenMaxCalls   *int     `json:"half_open_max_calls"`
	CallTimeout        *string  `json:"call_timeout"`
}

// ParseSettings reads the ConfigMap file for the given services. Unknown keys are rejected so a
// typo does not silently keep a default.
func ParseSettings(data []byte, services []string) (Settings, error) {
	var file fileSettings
	if err := yaml.UnmarshalStrict(data, &file); err != nil {
		return nil, fmt.Errorf("breaker settings: %w", err)
	}
	base, err := file.Defaults.apply(Defaults())
	if err != nil {
		return nil, fmt.Errorf("breaker settings, defaults: %w", err)
	}
	for name := range file.Services {
		if !contains(services, name) {
			return nil, fmt.Errorf("breaker settings: unknown service %q (known: %v)", name, services)
		}
	}
	settings := Settings{}
	for _, name := range services {
		s, err := file.Services[name].apply(base)
		if err != nil {
			return nil, fmt.Errorf("breaker settings, %s: %w", name, err)
		}
		settings[name] = s
	}
	return settings, nil
}

func (f fileService) apply(base ServiceSettings) (ServiceSettings, error) {
	out := base
	if f.FailureThreshold != nil {
		out.Breaker.FailureThreshold = *f.FailureThreshold
	}
	if f.ErrorRateThreshold != nil {
		out.Breaker.ErrorRateThreshold = *f.ErrorRateThreshold
	}
	if f.MinRequests != nil {
		out.Breaker.MinRequests = *f.MinRequests
	}
	if f.HalfOpenMaxCalls != nil {
		out.Breaker.HalfOpenMaxCalls = *f.HalfOpenMaxCalls
	}
	for _, d := range []struct {
		raw *string
		dst *time.Duration
		key string
	}{
		{f.Window, &out.Breaker.Window, "window"},
		{f.ResetTimeout, &out.Breaker.ResetTimeout, "reset_timeout"},
		{f.CallTimeout, &out.CallTimeout, "call_timeout"},
	} {
		if d.raw == nil {
			continue
		}
		parsed, err := time.ParseDuration(*d.raw)
		if err != nil {
			return out, fmt.Errorf("%s=%q is not a duration", d.key, *d.raw)
		}
		*d.dst = parsed
	}
	if err := out.Breaker.Validate(); err != nil {
		return out, err
	}
	if out.CallTimeout <= 0 {
		return out, errors.New("call_timeout must be positive")
	}
	return out, nil
}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}

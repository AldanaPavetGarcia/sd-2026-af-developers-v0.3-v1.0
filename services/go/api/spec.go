// Package apispec embeds the gateway's OpenAPI 3.0 contract (US-301 AC4, ADR-010 §4).
package apispec

import (
	_ "embed"

	"sigs.k8s.io/yaml"
)

//go:embed openapi.yaml
var specYAML []byte

// YAML returns the contract as written.
func YAML() []byte { return specYAML }

// JSON returns the contract converted to JSON, as served on /v1/openapi.json.
func JSON() ([]byte, error) { return yaml.YAMLToJSON(specYAML) }

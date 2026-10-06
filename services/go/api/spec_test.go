package apispec

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/getkin/kin-openapi/openapi3"
)

func TestSpecIsValidOpenAPI3(t *testing.T) {
	doc, err := openapi3.NewLoader().LoadFromData(YAML())
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if err := doc.Validate(context.Background()); err != nil {
		t.Fatalf("validate: %v", err)
	}
}

func TestJSONMatchesYAML(t *testing.T) {
	raw, err := JSON()
	if err != nil {
		t.Fatal(err)
	}
	var doc map[string]any
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatalf("JSON() is not JSON: %v", err)
	}
	if doc["openapi"] != "3.0.3" {
		t.Fatalf("openapi = %v", doc["openapi"])
	}
	if _, ok := doc["paths"].(map[string]any)["/v1/nodes"]; !ok {
		t.Fatal("/v1/nodes missing from the JSON document")
	}
}

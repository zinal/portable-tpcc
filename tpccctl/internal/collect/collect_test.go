package collect_test

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/tpccctl/internal/collect"
)

func TestCollectInstanceRejectsPayloadTraversal(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "source")
	if err := os.MkdirAll(source, 0755); err != nil {
		t.Fatal(err)
	}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "worker-a",
		Finalized:     true,
		Payloads: []collect.ArtifactPayloadEntry{
			{Path: "../secret", SHA256: "unused"},
		},
	}
	data, _ := json.Marshal(manifest)
	if err := os.WriteFile(filepath.Join(source, "artifact-manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
	}

	c := &collect.Collector{ResultRoot: filepath.Join(root, "results")}
	err := c.CollectInstance("run-1", "worker", "worker-a", source)
	if err == nil || !strings.Contains(err.Error(), "parent traversal") {
		t.Fatalf("expected traversal error, got %v", err)
	}
}

func TestCollectInstanceRejectsAbsolutePayloadPath(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "source")
	if err := os.MkdirAll(source, 0755); err != nil {
		t.Fatal(err)
	}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "worker-a",
		Finalized:     true,
		Payloads: []collect.ArtifactPayloadEntry{
			{Path: "/etc/passwd", SHA256: "unused"},
		},
	}
	data, _ := json.Marshal(manifest)
	if err := os.WriteFile(filepath.Join(source, "artifact-manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
	}

	c := &collect.Collector{ResultRoot: filepath.Join(root, "results")}
	err := c.CollectInstance("run-1", "worker", "worker-a", source)
	if err == nil || !strings.Contains(err.Error(), "must be relative") {
		t.Fatalf("expected absolute path error, got %v", err)
	}
}

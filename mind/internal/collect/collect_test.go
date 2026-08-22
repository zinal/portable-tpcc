package collect_test

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/mind/internal/collect"
)

func writeFinalizedSource(t *testing.T, source string, payloads map[string][]byte) {
	t.Helper()
	if err := os.MkdirAll(source, 0755); err != nil {
		t.Fatal(err)
	}
	var entries []collect.ArtifactPayloadEntry
	for name, data := range payloads {
		if err := os.WriteFile(filepath.Join(source, name), data, 0644); err != nil {
			t.Fatal(err)
		}
		sum := sha256.Sum256(data)
		entries = append(entries, collect.ArtifactPayloadEntry{
			Path:   name,
			Size:   int64(len(data)),
			SHA256: hex.EncodeToString(sum[:]),
		})
	}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "worker-a",
		Finalized:     true,
		Payloads:      entries,
	}
	data, err := json.Marshal(manifest)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(source, "artifact-manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
	}
}

func TestCollectInstanceCopiesPayloadsAtomically(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "source")
	payload := []byte("result-body")
	writeFinalizedSource(t, source, map[string][]byte{"result.json": payload})

	c := &collect.Collector{ResultRoot: filepath.Join(root, "results")}
	if err := c.CollectInstance("run-1", "loader", "ob-runner-1-l1", source); err != nil {
		t.Fatal(err)
	}

	dest := filepath.Join(root, "results", "run-1", "raw", "loader", "ob-runner-1-l1")
	got, err := os.ReadFile(filepath.Join(dest, "result.json"))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(payload) {
		t.Fatalf("payload=%q, want %q", got, payload)
	}
	if _, err := os.Stat(dest + ".tmp"); !os.IsNotExist(err) {
		t.Fatalf("staging dir left behind: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dest, ".tmp")); !os.IsNotExist(err) {
		t.Fatalf("nested staging dir left behind: %v", err)
	}
}

func TestCollectInstanceReplacesExistingDestination(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "source")
	writeFinalizedSource(t, source, map[string][]byte{"result.json": []byte("v1")})

	c := &collect.Collector{ResultRoot: filepath.Join(root, "results")}
	if err := c.CollectInstance("run-1", "loader", "ob-runner-1-l1", source); err != nil {
		t.Fatal(err)
	}
	writeFinalizedSource(t, source, map[string][]byte{"result.json": []byte("v2")})
	if err := c.CollectInstance("run-1", "loader", "ob-runner-1-l1", source); err != nil {
		t.Fatal(err)
	}

	dest := filepath.Join(root, "results", "run-1", "raw", "loader", "ob-runner-1-l1")
	got, err := os.ReadFile(filepath.Join(dest, "result.json"))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "v2" {
		t.Fatalf("payload=%q, want v2", got)
	}
}

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

func TestCollectInstanceRejectsUnsupportedPayloadName(t *testing.T) {
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
			{Path: "notes.txt", SHA256: "unused"},
		},
	}
	data, _ := json.Marshal(manifest)
	if err := os.WriteFile(filepath.Join(source, "artifact-manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
	}

	c := &collect.Collector{ResultRoot: filepath.Join(root, "results")}
	err := c.CollectInstance("run-1", "worker", "worker-a", source)
	if err == nil || !strings.Contains(err.Error(), "unsupported artifact payload") {
		t.Fatalf("expected unsupported payload error, got %v", err)
	}
}

func TestCollectInstanceRejectsSymlinkEscape(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "source")
	outside := filepath.Join(root, "outside.txt")
	if err := os.MkdirAll(source, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(outside, []byte("secret"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(source, "result.json")); err != nil {
		t.Fatal(err)
	}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "worker-a",
		Finalized:     true,
		Payloads: []collect.ArtifactPayloadEntry{
			{Path: "result.json", SHA256: "unused"},
		},
	}
	data, _ := json.Marshal(manifest)
	if err := os.WriteFile(filepath.Join(source, "artifact-manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
	}

	c := &collect.Collector{ResultRoot: filepath.Join(root, "results")}
	err := c.CollectInstance("run-1", "worker", "worker-a", source)
	if err == nil || !strings.Contains(err.Error(), "escapes base") {
		t.Fatalf("expected symlink escape error, got %v", err)
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

func TestHasCollectionManifest(t *testing.T) {
	root := t.TempDir()
	runID := "run-1"
	if collect.HasCollectionManifest(root, runID) {
		t.Fatal("missing manifest must be reported absent")
	}
	path := collect.CollectionManifestPath(root, runID)
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(path, 0755); err != nil {
		t.Fatal(err)
	}
	if collect.HasCollectionManifest(root, runID) {
		t.Fatal("directory at manifest path must not count as collected")
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("{}\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if !collect.HasCollectionManifest(root, runID) {
		t.Fatal("regular file at manifest path must count as collected")
	}
}

package profile_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/tools/tpccctl/internal/profile"
)

func TestParse_rejectsUnknownFields(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	// Typo must not be ignored (otherwise default exponential would apply silently).
	patched := strings.Replace(
		string(data),
		"runtime:\n  pacing: enabled\n",
		"runtime:\n  think_time_distribtion: compatibility\n  pacing: enabled\n",
		1,
	)
	if patched == string(data) {
		t.Fatal("failed to inject unknown field into test fixture")
	}
	_, err = profile.Parse([]byte(patched))
	if err == nil || !strings.Contains(err.Error(), "think_time_distribtion") {
		t.Fatalf("expected unknown-field error, got %v", err)
	}
}

func TestParse_acceptsValidProfile(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if p.Metadata.Name != "test-profile" {
		t.Fatalf("name=%q", p.Metadata.Name)
	}
}

func TestParse_acceptsKnownThinkTimeDistribution(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	patched := strings.Replace(
		string(data),
		"runtime:\n  pacing: enabled\n",
		"runtime:\n  think_time_distribution: compatibility\n  pacing: enabled\n",
		1,
	)
	p, err := profile.Parse([]byte(patched))
	if err != nil {
		t.Fatal(err)
	}
	if p.Runtime.ThinkTimeDistribution != "compatibility" {
		t.Fatalf("think_time_distribution=%q", p.Runtime.ThinkTimeDistribution)
	}
}

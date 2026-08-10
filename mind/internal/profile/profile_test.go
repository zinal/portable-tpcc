package profile_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/mind/internal/profile"
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

func TestParse_rejectsHostsSection(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	patched := strings.Replace(
		string(data),
		"paths:\n",
		"hosts:\n  node1:\n    address: 10.10.0.1\npaths:\n",
		1,
	)
	if patched == string(data) {
		t.Fatal("failed to inject hosts section into test fixture")
	}
	_, err = profile.Parse([]byte(patched))
	if err == nil || !strings.Contains(err.Error(), "hosts") {
		t.Fatalf("expected hosts field rejection, got %v", err)
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

func TestParse_rejectsUnsupportedHistogramKnobs(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	// HDR-style knobs are not part of linear_exp; KnownFields must reject them.
	patched := strings.Replace(
		string(data),
		"runtime:\n  pacing: enabled\n",
		"runtime:\n  pacing: enabled\n  histogram:\n    unit: us\n    highest: 1000\n    lowest: 1\n    significant_figures: 3\n",
		1,
	)
	if patched == string(data) {
		t.Fatal("failed to inject histogram knobs into test fixture")
	}
	_, err = profile.Parse([]byte(patched))
	if err == nil {
		t.Fatal("expected unknown-field error for lowest/significant_figures")
	}
	if !strings.Contains(err.Error(), "lowest") && !strings.Contains(err.Error(), "significant_figures") {
		t.Fatalf("expected error mentioning removed histogram fields, got %v", err)
	}
}

func TestParse_acceptsHistogramUnitHighest(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	patched := strings.Replace(
		string(data),
		"runtime:\n  pacing: enabled\n",
		"runtime:\n  pacing: enabled\n  histogram:\n    unit: ms\n    highest: 60000\n",
		1,
	)
	p, err := profile.Parse([]byte(patched))
	if err != nil {
		t.Fatal(err)
	}
	if p.Runtime.Histogram.Unit != "ms" {
		t.Fatalf("unit=%q", p.Runtime.Histogram.Unit)
	}
	if p.Runtime.Histogram.Highest != 60000 {
		t.Fatalf("highest=%d", p.Runtime.Histogram.Highest)
	}
}

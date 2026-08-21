package collect_test

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"portable-tpcc/mind/internal/canonical"
	"portable-tpcc/mind/internal/collect"
	"portable-tpcc/mind/internal/config"
)

func TestFromWorkerAssignments(t *testing.T) {
	cp := collect.FromWorkerAssignments([]config.WorkerAssignmentJSON{
		{Instance: "worker-b", Host: "host-b", Threads: 8, MaxInflight: 128},
		{Instance: "worker-a", Host: "host-a", Threads: 8, MaxInflight: 128},
	})
	if cp.Clients != 2 {
		t.Fatalf("clients=%d, want 2", cp.Clients)
	}
	if cp.ThreadsPerWorker != 8 || cp.MaxInflightPerWorker != 128 {
		t.Fatalf("profile-level %+v", cp)
	}
	if len(cp.Workers) != 2 || cp.Workers[0].Instance != "worker-b" {
		t.Fatalf("workers order %+v", cp.Workers)
	}
}

func TestFromWorkerResultAssignmentsSortsInstances(t *testing.T) {
	cp := collect.FromWorkerResultAssignments(map[string]map[string]interface{}{
		"worker-b": {"instance": "worker-b", "threads": 4.0, "max_inflight": 64.0},
		"worker-a": {"instance": "worker-a", "host": "h-a", "threads": 4.0, "max_inflight": 64.0},
	})
	if cp.Clients != 2 {
		t.Fatalf("clients=%d", cp.Clients)
	}
	if cp.Workers[0].Instance != "worker-a" || cp.Workers[1].Instance != "worker-b" {
		t.Fatalf("expected sorted workers, got %+v", cp.Workers)
	}
	if cp.Workers[0].Host != "h-a" || cp.Workers[0].Threads != 4 {
		t.Fatalf("worker-a %+v", cp.Workers[0])
	}
}

func TestWriteAndReadClientParallelism(t *testing.T) {
	root := t.TempDir()
	runID := "run-cp"
	cp := collect.FromWorkerAssignments([]config.WorkerAssignmentJSON{
		{Instance: "worker-a", Host: "host-a", Threads: 10, MaxInflight: 256},
	})
	entry, err := collect.WriteClientParallelism(root, runID, cp)
	if err != nil {
		t.Fatal(err)
	}
	if entry.Path != collect.ClientParallelismFileName {
		t.Fatalf("path=%q", entry.Path)
	}
	gotSHA, err := canonical.SHA256File(filepath.Join(root, runID, collect.ClientParallelismFileName))
	if err != nil {
		t.Fatal(err)
	}
	if entry.SHA256 != gotSHA {
		t.Fatalf("sha mismatch %s vs %s", entry.SHA256, gotSHA)
	}
	loaded, err := collect.ReadClientParallelism(root, runID)
	if err != nil {
		t.Fatal(err)
	}
	if loaded.Clients != 1 || loaded.ThreadsPerWorker != 10 || loaded.MaxInflightPerWorker != 256 {
		t.Fatalf("loaded %+v", loaded)
	}
	m := loaded.ToMap()
	data, _ := json.Marshal(m)
	var roundtrip collect.ClientParallelism
	if err := json.Unmarshal(data, &roundtrip); err != nil {
		t.Fatal(err)
	}
	if roundtrip.Clients != 1 {
		t.Fatalf("roundtrip %+v", roundtrip)
	}
	if _, err := os.Stat(filepath.Join(root, runID, collect.ClientParallelismFileName)); err != nil {
		t.Fatal(err)
	}
}

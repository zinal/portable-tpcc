package config_test

import (
	"testing"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/profile"
)

func minimalProfileForWorkerDefaults(t *testing.T, runtime profile.Runtime) *profile.Profile {
	t.Helper()
	seed := int64(1)
	return &profile.Profile{
		Metadata: profile.Metadata{Name: "worker-defaults"},
		Database: profile.Database{
			DBMS:     "pgsql",
			Endpoint: "localhost:5432",
			Database: "tpcc",
			Path:     "tpcc",
			User:     "postgres",
		},
		Scale:   profile.Scale{Warehouses: 100},
		Data:    profile.Data{Seed: &seed, BatchRows: 100},
		Loaders: []profile.NamedHost{{Name: "loader-a", Host: "h1"}},
		Workers: []profile.NamedHost{
			{Name: "worker-a", Host: "h1"},
			{Name: "worker-b", Host: "h2"},
		},
		Phases: profile.Phases{
			StartLead:        "1s",
			RampUp:           "1s",
			Measurement:      "10m",
			TransactionDrain: "1s",
			StopGrace:        "1s",
			MaxClockSkew:     "100ms",
		},
		Runtime: runtime,
	}
}

func TestBuildRunConfig_omittedWorkerThreadsStayAuto(t *testing.T) {
	p := minimalProfileForWorkerDefaults(t, profile.Runtime{})
	rc, err := config.BuildRunConfig(config.BuildInput{Profile: p, RunID: "run-auto"})
	if err != nil {
		t.Fatal(err)
	}
	if len(rc.WorkerAssignment) != 2 {
		t.Fatalf("workers=%d", len(rc.WorkerAssignment))
	}
	for _, w := range rc.WorkerAssignment {
		if w.Threads != 0 {
			t.Fatalf("%s threads=%d, want 0 (auto for ComputeRunLayout)", w.Instance, w.Threads)
		}
		if w.MaxInflight != config.DefaultMaxInflightPerWorker {
			t.Fatalf("%s max_inflight=%d, want %d", w.Instance, w.MaxInflight, config.DefaultMaxInflightPerWorker)
		}
	}
}

func TestBuildRunConfig_explicitWorkerParallelismPinned(t *testing.T) {
	p := minimalProfileForWorkerDefaults(t, profile.Runtime{
		ThreadsPerWorker:     8,
		MaxInflightPerWorker: 256,
	})
	rc, err := config.BuildRunConfig(config.BuildInput{Profile: p, RunID: "run-pin"})
	if err != nil {
		t.Fatal(err)
	}
	for _, w := range rc.WorkerAssignment {
		if w.Threads != 8 {
			t.Fatalf("%s threads=%d, want 8", w.Instance, w.Threads)
		}
		if w.MaxInflight != 256 {
			t.Fatalf("%s max_inflight=%d, want 256", w.Instance, w.MaxInflight)
		}
	}
}

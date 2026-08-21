package config

import (
	"reflect"
	"testing"
)

func TestResolveCheckConcurrency(t *testing.T) {
	t.Parallel()
	cases := []struct {
		warehouses int
		configured int
		want       int
	}{
		{warehouses: 10, configured: 0, want: 10},
		{warehouses: 100, configured: 0, want: DefaultCheckConcurrencyCap},
		{warehouses: 10, configured: 4, want: 4},
		{warehouses: 0, configured: 0, want: 1},
		{warehouses: 10, configured: 1, want: 1},
	}
	for _, tc := range cases {
		got := ResolveCheckConcurrency(tc.warehouses, tc.configured)
		if got != tc.want {
			t.Fatalf("ResolveCheckConcurrency(%d, %d)=%d, want %d",
				tc.warehouses, tc.configured, got, tc.want)
		}
	}
}

func TestEffectiveCheckConcurrency(t *testing.T) {
	t.Parallel()
	cli := 16
	got := EffectiveCheckConcurrency(10, 4, &cli)
	if got != 16 {
		t.Fatalf("CLI override=%d, want 16", got)
	}
	zero := 0
	got = EffectiveCheckConcurrency(10, 4, &zero)
	if got != 10 {
		t.Fatalf("CLI 0 (auto)=%d, want 10", got)
	}
	got = EffectiveCheckConcurrency(10, 0, nil)
	if got != 10 {
		t.Fatalf("omit=%d, want 10", got)
	}
}

func TestBuildPlanSnapshotResolvesCheckThreads(t *testing.T) {
	t.Parallel()
	rc := &RunConfig{
		Scale:   ScaleBlock{Warehouses: 10},
		Runtime: RunRuntime{CheckConcurrency: 0},
	}
	plan := BuildPlanSnapshot(rc, nil)
	want := []string{
		"check",
		"--run-config", "run-config.json",
		"--instance", "check-0",
		"--after-import",
		"--threads=10",
	}
	if !reflect.DeepEqual(plan.CheckArgvImport, want) {
		t.Fatalf("CheckArgvImport=%v, want %v", plan.CheckArgvImport, want)
	}
	cli := 16
	overridden := BuildPlanSnapshot(rc, &cli)
	if got := overridden.CheckArgvImport; len(got) == 0 || got[len(got)-1] != "--threads=16" {
		t.Fatalf("CLI CheckArgvImport=%v, want --threads=16", overridden.CheckArgvImport)
	}
}

func TestBuildPlanSnapshotPassesThreadsToWorkerAndLoader(t *testing.T) {
	t.Parallel()
	rc := &RunConfig{
		Scale: ScaleBlock{Warehouses: 10},
		LoadAssignment: []LoadAssignmentJSON{{
			Instance: "loader-a",
			Host:     "h1",
			Threads:  2,
		}},
		WorkerAssignment: []WorkerAssignmentJSON{{
			Instance: "worker-a",
			Host:     "h1",
			Threads:  2,
		}},
	}
	plan := BuildPlanSnapshot(rc, nil)
	if got := plan.WorkerArgv["worker-a"]; len(got) != 5 {
		t.Fatalf("unset worker argv %v, want 5 args without --threads", got)
	}
	if got := plan.LoaderArgv["loader-a"]; len(got) != 5 {
		t.Fatalf("unset loader argv %v, want 5 args without --threads", got)
	}
	cli := 64
	overridden := BuildPlanSnapshot(rc, &cli)
	if got := overridden.WorkerArgv["worker-a"]; len(got) == 0 || got[len(got)-1] != "--threads=64" {
		t.Fatalf("worker argv %v, want --threads=64", got)
	}
	if got := overridden.LoaderArgv["loader-a"]; len(got) == 0 || got[len(got)-1] != "--threads=64" {
		t.Fatalf("loader argv %v, want --threads=64", got)
	}
	if overridden.WorkerAssignment[0].Threads != 2 {
		t.Fatalf("assignment threads=%d, want 2 (run-config unchanged)", overridden.WorkerAssignment[0].Threads)
	}
	zero := 0
	auto := BuildPlanSnapshot(rc, &zero)
	if got := auto.WorkerArgv["worker-a"]; len(got) == 0 || got[len(got)-1] != "--threads=0" {
		t.Fatalf("auto worker argv %v, want --threads=0", got)
	}
}

func TestCheckArgvIncludesThreads(t *testing.T) {
	t.Parallel()
	got := CheckArgv("run-config.json", "check-0", "after-import", 10)
	want := []string{
		"check",
		"--run-config", "run-config.json",
		"--instance", "check-0",
		"--after-import",
		"--threads=10",
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("CheckArgv=%v, want %v", got, want)
	}
	serial := CheckArgv("run-config.json", "check-0", "after-test", 0)
	wantSerial := []string{
		"check",
		"--run-config", "run-config.json",
		"--instance", "check-0",
		"--after-test",
	}
	if !reflect.DeepEqual(serial, wantSerial) {
		t.Fatalf("serial CheckArgv=%v, want %v", serial, wantSerial)
	}
}

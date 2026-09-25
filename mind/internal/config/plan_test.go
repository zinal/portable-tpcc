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
	plan := BuildPlanSnapshot(rc, nil, nil)
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
	overridden := BuildPlanSnapshot(rc, &cli, nil)
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
	plan := BuildPlanSnapshot(rc, nil, nil)
	if got := plan.WorkerArgv["worker-a"]; len(got) != 5 {
		t.Fatalf("unset worker argv %v, want 5 args without --threads", got)
	}
	if got := plan.LoaderArgv["loader-a"]; len(got) != 5 {
		t.Fatalf("unset loader argv %v, want 5 args without --threads", got)
	}
	cli := 64
	overridden := BuildPlanSnapshot(rc, &cli, nil)
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
	auto := BuildPlanSnapshot(rc, &zero, nil)
	if got := auto.WorkerArgv["worker-a"]; len(got) == 0 || got[len(got)-1] != "--threads=0" {
		t.Fatalf("auto worker argv %v, want --threads=0", got)
	}
}

func TestBuildPlanSnapshotPassesMaxInflightToWorkerOnly(t *testing.T) {
	t.Parallel()
	rc := &RunConfig{
		Scale: ScaleBlock{Warehouses: 10},
		LoadAssignment: []LoadAssignmentJSON{{
			Instance: "loader-a",
			Host:     "h1",
			Threads:  2,
		}},
		WorkerAssignment: []WorkerAssignmentJSON{{
			Instance:    "worker-a",
			Host:        "h1",
			Threads:     2,
			MaxInflight: 100,
		}},
	}
	plan := BuildPlanSnapshot(rc, nil, nil)
	if got := plan.WorkerArgv["worker-a"]; len(got) != 5 {
		t.Fatalf("unset worker argv %v, want 5 args without --max-inflight", got)
	}
	if got := plan.LoaderArgv["loader-a"]; len(got) != 5 {
		t.Fatalf("loader argv %v, want no max-inflight flag", got)
	}
	inflight := 256
	overridden := BuildPlanSnapshot(rc, nil, &inflight)
	if got := overridden.WorkerArgv["worker-a"]; len(got) == 0 || got[len(got)-1] != "--max-inflight=256" {
		t.Fatalf("worker argv %v, want --max-inflight=256", got)
	}
	if got := overridden.LoaderArgv["loader-a"]; len(got) != 5 {
		t.Fatalf("loader argv %v, want profile threads only", got)
	}
	if overridden.WorkerAssignment[0].MaxInflight != 100 {
		t.Fatalf("assignment max_inflight=%d, want 100 (run-config unchanged)", overridden.WorkerAssignment[0].MaxInflight)
	}
	threads := 8
	both := BuildPlanSnapshot(rc, &threads, &inflight)
	got := both.WorkerArgv["worker-a"]
	if len(got) < 2 || got[len(got)-2] != "--threads=8" || got[len(got)-1] != "--max-inflight=256" {
		t.Fatalf("worker argv %v, want --threads=8 --max-inflight=256", got)
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

func TestDebugArgvIncludesRepeats(t *testing.T) {
	t.Parallel()
	got := DebugArgv("run-config.json", "debug-0", 0)
	want := []string{
		"debug",
		"--run-config", "run-config.json",
		"--instance", "debug-0",
		"--repeats=10",
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("DebugArgv default=%v, want %v", got, want)
	}
	got = DebugArgv("run-config.json", "debug-0", 3)
	if got[len(got)-1] != "--repeats=3" {
		t.Fatalf("DebugArgv override=%v, want --repeats=3", got)
	}
	if EffectiveDebugRepeats(nil) != DefaultDebugRepeats {
		t.Fatalf("EffectiveDebugRepeats(nil)=%d", EffectiveDebugRepeats(nil))
	}
	n := 4
	if got := EffectiveDebugRepeats(&n); got != 4 {
		t.Fatalf("EffectiveDebugRepeats=&4 = %d", got)
	}
}

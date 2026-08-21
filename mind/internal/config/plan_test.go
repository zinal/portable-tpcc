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

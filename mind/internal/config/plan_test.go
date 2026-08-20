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
	serial := CheckArgv("run-config.json", "check-0", "after-run", 0)
	wantSerial := []string{
		"check",
		"--run-config", "run-config.json",
		"--instance", "check-0",
		"--after-run",
	}
	if !reflect.DeepEqual(serial, wantSerial) {
		t.Fatalf("serial CheckArgv=%v, want %v", serial, wantSerial)
	}
}

package cli

import "testing"

func TestRun_helpWithoutProfile(t *testing.T) {
	for _, args := range [][]string{
		{"--help"},
		{"-h"},
		{"help"},
		{"validate", "--help"},
		{"plan", "-h"},
	} {
		if code := Run(args); code != 0 {
			t.Fatalf("Run(%v)=%d, want 0", args, code)
		}
	}
}

func TestRun_missingProfile(t *testing.T) {
	if code := Run([]string{"validate"}); code != 2 {
		t.Fatalf("Run([validate])=%d, want 2", code)
	}
}

func TestRun_emptyArgsShowsUsage(t *testing.T) {
	if code := Run(nil); code != 2 {
		t.Fatalf("Run(nil)=%d, want 2", code)
	}
}

func TestRun_warehousesIncreaseRejected(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	code := Run([]string{
		"validate",
		"--profile", profilePath,
		"--warehouses", "11",
	})
	if code == 0 {
		t.Fatal("expected non-zero exit for --warehouses above profile")
	}
}

func TestRun_overrideFlagsAcceptedOnValidate(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	code := Run([]string{
		"validate",
		"--profile", profilePath,
		"--warehouses", "1",
		"--ramp-up", "10s",
		"--measurement", "1m",
	})
	if code != 0 {
		t.Fatalf("validate with overrides=%d, want 0", code)
	}
}

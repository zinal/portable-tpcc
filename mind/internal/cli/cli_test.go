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

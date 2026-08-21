package cli

import (
	"io"
	"os"
	"strings"
	"testing"
)

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
		"--leave-processes",
		"--threads", "16",
	})
	if code != 0 {
		t.Fatalf("validate with overrides=%d, want 0", code)
	}
}

func TestRun_helpMentionsLeaveProcesses(t *testing.T) {
	old := os.Stdout
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	os.Stdout = w
	code := Run([]string{"--help"})
	_ = w.Close()
	os.Stdout = old
	if code != 0 {
		t.Fatalf("help=%d, want 0", code)
	}
	out, err := io.ReadAll(r)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(out), "--leave-processes") {
		t.Fatalf("help missing --leave-processes:\n%s", out)
	}
	if !strings.Contains(string(out), "--threads") {
		t.Fatalf("help missing --threads:\n%s", out)
	}
	if !strings.Contains(string(out), "test        Arm workers") {
		t.Fatalf("help missing test command:\n%s", out)
	}
	if !strings.Contains(string(out), "start       Alias for test") {
		t.Fatalf("help missing start alias:\n%s", out)
	}
}

func TestRun_threadsFlagAcceptedOnValidate(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	for _, args := range [][]string{
		{"validate", "--profile", profilePath, "--threads", "16"},
		{"validate", "--profile=" + profilePath, "--threads=16"},
	} {
		if code := Run(args); code != 0 {
			t.Fatalf("Run(%v)=%d, want 0", args, code)
		}
	}
}

func TestRun_unknownFlagRejected(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	stderr := captureStderr(t, func() {
		code := Run([]string{"validate", "--profile", profilePath, "--not-a-real-flag"})
		if code != 2 {
			t.Fatalf("unknown flag exit=%d, want 2", code)
		}
	})
	if !strings.Contains(stderr, "unknown flag --not-a-real-flag") {
		t.Fatalf("stderr=%q", stderr)
	}
}

func TestRun_unexpectedArgumentRejected(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	stderr := captureStderr(t, func() {
		code := Run([]string{"validate", "--profile", profilePath, "leftover"})
		if code != 2 {
			t.Fatalf("unexpected arg exit=%d, want 2", code)
		}
	})
	if !strings.Contains(stderr, `unexpected argument "leftover"`) {
		t.Fatalf("stderr=%q", stderr)
	}
}

func TestRun_threadsNegativeRejected(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	stderr := captureStderr(t, func() {
		code := Run([]string{"validate", "--profile", profilePath, "--threads", "-1"})
		if code != 2 {
			t.Fatalf("negative --threads exit=%d, want 2", code)
		}
	})
	if !strings.Contains(stderr, "--threads must not be negative") {
		t.Fatalf("stderr=%q", stderr)
	}
}

func captureStderr(t *testing.T, fn func()) string {
	t.Helper()
	old := os.Stderr
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	os.Stderr = w
	fn()
	_ = w.Close()
	os.Stderr = old
	out, err := io.ReadAll(r)
	if err != nil {
		t.Fatal(err)
	}
	return string(out)
}

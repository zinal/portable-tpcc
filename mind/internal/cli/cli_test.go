package cli

import (
	"io"
	"os"
	"path/filepath"
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
	if !strings.Contains(string(out), "Override worker/loader threads") {
		t.Fatalf("help missing worker/loader --threads meaning:\n%s", out)
	}
	if !strings.Contains(string(out), "--max-inflight <n>") {
		t.Fatalf("help missing --max-inflight:\n%s", out)
	}
	if !strings.Contains(string(out), "test        Arm workers") {
		t.Fatalf("help missing test command:\n%s", out)
	}
	if !strings.Contains(string(out), "debug       Sequential probe") {
		t.Fatalf("help missing debug command:\n%s", out)
	}
	if !strings.Contains(string(out), "--repeats <n>") {
		t.Fatalf("help missing --repeats:\n%s", out)
	}
	if !strings.Contains(string(out), "start       Alias for test") {
		t.Fatalf("help missing start alias:\n%s", out)
	}
	if !strings.Contains(string(out), "collects first if needed") {
		t.Fatalf("help missing consolidate auto-collect:\n%s", out)
	}
	if !strings.Contains(string(out), "drop        Drop TPC-C objects") {
		t.Fatalf("help missing drop command:\n%s", out)
	}
	if !strings.Contains(string(out), "Remove run artifacts on all hosts including control") {
		t.Fatalf("help missing cleanup artifact-only wording:\n%s", out)
	}
	if !strings.Contains(string(out), "--insecure-ignore-host-key") {
		t.Fatalf("help missing --insecure-ignore-host-key:\n%s", out)
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

func TestRun_insecureIgnoreHostKeyAcceptedOnValidate(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	for _, args := range [][]string{
		{"validate", "--profile", profilePath, "--insecure-ignore-host-key"},
		{"validate", "--profile=" + profilePath, "--insecure-ignore-host-key=true"},
	} {
		if code := Run(args); code != 0 {
			t.Fatalf("Run(%v)=%d, want 0", args, code)
		}
	}
}

func TestRun_insecureIgnoreAllowsMissingKnownHosts(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	data, err := os.ReadFile(profilePath)
	if err != nil {
		t.Fatal(err)
	}
	stripped := strings.ReplaceAll(string(data), "  known_hosts: ~/.ssh/known_hosts\n", "")
	if stripped == string(data) {
		t.Fatal("test fixture missing known_hosts line")
	}
	if err := os.WriteFile(profilePath, []byte(stripped), 0644); err != nil {
		t.Fatal(err)
	}
	if code := Run([]string{"validate", "--profile", profilePath}); code == 0 {
		t.Fatal("expected validate to fail without known_hosts")
	}
	if code := Run([]string{"validate", "--profile", profilePath, "--insecure-ignore-host-key"}); code != 0 {
		t.Fatalf("expected validate to pass with --insecure-ignore-host-key, got %d", code)
	}
}

func TestRun_repeatsNonPositiveRejected(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	stderr := captureStderr(t, func() {
		code := Run([]string{"validate", "--profile", profilePath, "--repeats", "0"})
		if code != 2 {
			t.Fatalf("zero --repeats exit=%d, want 2", code)
		}
	})
	if !strings.Contains(stderr, "--repeats must be greater than zero") {
		t.Fatalf("stderr=%q", stderr)
	}
}

func TestRun_maxInflightFlagAcceptedOnValidate(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	for _, args := range [][]string{
		{"validate", "--profile", profilePath, "--max-inflight", "256"},
		{"validate", "--profile=" + profilePath, "--max-inflight=64"},
	} {
		if code := Run(args); code != 0 {
			t.Fatalf("Run(%v)=%d, want 0", args, code)
		}
	}
}

func TestRun_maxInflightNonPositiveRejected(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	for _, raw := range []string{"0", "-1"} {
		stderr := captureStderr(t, func() {
			code := Run([]string{"validate", "--profile", profilePath, "--max-inflight", raw})
			if code != 2 {
				t.Fatalf("--max-inflight %s exit=%d, want 2", raw, code)
			}
		})
		if !strings.Contains(stderr, "--max-inflight must be greater than zero") {
			t.Fatalf("stderr=%q", stderr)
		}
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

func TestRun_consolidateWithoutRunLeavesRunIDEmpty(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	stderr := captureStderr(t, func() {
		code := Run([]string{"consolidate", "--profile", profilePath})
		if code == 0 {
			t.Fatal("expected consolidate without a run to fail")
		}
	})
	if !strings.Contains(stderr, "refusing to allocate") {
		t.Fatalf("stderr=%q", stderr)
	}
	entries, err := os.ReadDir(filepath.Join(dir, "state", "runs"))
	if err != nil && !os.IsNotExist(err) {
		t.Fatal(err)
	}
	if len(entries) != 0 {
		t.Fatalf("consolidate created %d run dir(s)", len(entries))
	}
}

func TestRun_consolidateBeforeTestKeepsPlannedState(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	if code := Run([]string{"plan", "--profile", profilePath}); code != 0 {
		t.Fatalf("plan=%d", code)
	}
	runs := filepath.Join(dir, "state", "runs")
	entries, err := os.ReadDir(runs)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 {
		t.Fatalf("runs=%d, want 1", len(entries))
	}
	statePath := filepath.Join(runs, entries[0].Name(), "run-state.json")
	before, err := os.ReadFile(statePath)
	if err != nil {
		t.Fatal(err)
	}
	stderr := captureStderr(t, func() {
		code := Run([]string{"consolidate", "--profile", profilePath})
		if code == 0 {
			t.Fatal("expected consolidate before test to fail")
		}
	})
	if !strings.Contains(stderr, "has not finished test") {
		t.Fatalf("stderr=%q", stderr)
	}
	after, err := os.ReadFile(statePath)
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != string(before) {
		t.Fatalf("state changed:\n%s", after)
	}
	again, err := os.ReadDir(runs)
	if err != nil {
		t.Fatal(err)
	}
	if len(again) != 1 || again[0].Name() != entries[0].Name() {
		t.Fatalf("run set changed")
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

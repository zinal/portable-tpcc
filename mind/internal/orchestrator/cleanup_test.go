package orchestrator

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/remote"
	"portable-tpcc/mind/internal/state"
)

func TestCleanupNeedsByState(t *testing.T) {
	cases := []struct {
		state        string
		wantRemote   bool
		wantDatabase bool
	}{
		{state.StatePlanned, false, false},
		{state.StateDeploying, true, false},
		{state.StateSchema, true, true},
		{state.StateLoading, true, true},
		{state.StateMeasuring, true, true},
		{state.StateCompleted, true, true},
		{state.StateFailed, true, true},
		{state.StateStopping, true, true},
	}
	for _, tc := range cases {
		if got := cleanupNeedsRemote(tc.state); got != tc.wantRemote {
			t.Fatalf("state %s remote=%v, want %v", tc.state, got, tc.wantRemote)
		}
		if got := cleanupNeedsDB(tc.state); got != tc.wantDatabase {
			t.Fatalf("state %s db=%v, want %v", tc.state, got, tc.wantDatabase)
		}
	}
}

func TestAssertSafeRemoteRunDir(t *testing.T) {
	if err := assertSafeRemoteRunDir("/tmp/runs/run-1", "run-1"); err != nil {
		t.Fatal(err)
	}
	if err := assertSafeRemoteRunDir("/", "run-1"); err == nil {
		t.Fatal("expected error for /")
	}
	if err := assertSafeRemoteRunDir("/tmp/runs/other", "run-1"); err == nil {
		t.Fatal("expected error for mismatched base")
	}
}

func TestValidateCleanupRunID(t *testing.T) {
	if err := validateCleanupRunID("run-1"); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"..", ".", "a/b", `a\b`, "a..b"} {
		if err := validateCleanupRunID(id); err == nil {
			t.Fatalf("expected invalid run_id %q", id)
		}
	}
}

func TestCleanArgv(t *testing.T) {
	got := config.CleanArgv("run-config.json", "clean-0")
	want := []string{"clean", "--run-config", "run-config.json", "--instance", "clean-0"}
	if len(got) != len(want) {
		t.Fatalf("argv=%v", got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("argv=%v, want %v", got, want)
		}
	}
}

func TestRemoveRemoteRunDirs(t *testing.T) {
	remoteRoot := t.TempDir()
	o := &Orchestrator{
		Expanded: config.ExpandedPaths{RemoteRoot: remoteRoot},
	}
	ctx := &Context{RunID: "run-abc"}
	sess := &fakeSession{files: map[string][]byte{
		filepath.Join(remoteRoot, "run-abc"): []byte("dir"),
	}}
	if err := o.removeRemoteRunDirs(ctx, map[string]remote.Session{"host-a": sess}); err != nil {
		t.Fatal(err)
	}
	if len(sess.removedAll) != 1 || sess.removedAll[0] != filepath.Join(remoteRoot, "run-abc") {
		t.Fatalf("removedAll=%v", sess.removedAll)
	}
}

func TestResolveCleanupRunIDIncludesTerminal(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCleanupProfile(t, dir)
	o, err := New(Options{ProfilePath: profilePath, RunID: "run-done"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	rs, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	rs.State = state.StateCompleted
	rs.UpdatedAt = time.Now().UTC().Format(time.RFC3339)
	if err := o.StateStore.Save(rs); err != nil {
		t.Fatal(err)
	}

	o2, err := New(Options{ProfilePath: profilePath})
	if err != nil {
		t.Fatal(err)
	}
	got, err := o2.ResolveCleanupRunID()
	if err != nil {
		t.Fatal(err)
	}
	if got != "run-done" {
		t.Fatalf("run_id=%q, want run-done", got)
	}
}

func TestCleanupPlannedRemovesLocalStateAndResults(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCleanupProfile(t, dir)
	o, err := New(Options{ProfilePath: profilePath, RunID: "run-planned"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	resultsDir := filepath.Join(o.Expanded.ResultRoot, ctx.RunID)
	if err := os.MkdirAll(filepath.Join(resultsDir, "raw"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := o.Cleanup(ctx, true); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(o.StateStore.RunDir(ctx.RunID)); !os.IsNotExist(err) {
		t.Fatalf("run state still present: %v", err)
	}
	if _, err := os.Stat(resultsDir); !os.IsNotExist(err) {
		t.Fatalf("results still present: %v", err)
	}
}

func TestCleanupSchemaDropsRemoteAndRunsClean(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCleanupProfile(t, dir)
	t.Setenv("TPCC_PASSWORD", "secret")

	o, err := New(Options{ProfilePath: profilePath, RunID: "run-schema"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	rs, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	rs.State = state.StateSchema
	if err := o.StateStore.Save(rs); err != nil {
		t.Fatal(err)
	}

	runDir := filepath.Join(o.Expanded.RemoteRoot, ctx.RunID)
	if err := os.MkdirAll(runDir, 0755); err != nil {
		t.Fatal(err)
	}
	// Local runtimeRoot Abs-expands remote_root; mirror that.
	absRemote, err := filepath.Abs(o.Expanded.RemoteRoot)
	if err != nil {
		t.Fatal(err)
	}
	absRunDir := filepath.Join(absRemote, ctx.RunID)
	if err := os.MkdirAll(absRunDir, 0755); err != nil {
		t.Fatal(err)
	}
	binPath := filepath.Join(absRunDir, ctx.RunConfig.Binary)
	script := `#!/bin/sh
set -e
cmd="$1"
instance="clean-0"
prev=""
for a in "$@"; do
  if [ "$prev" = "--instance" ]; then instance="$a"; fi
  prev="$a"
done
mkdir -p "$cmd/$instance"
printf '{"pid":%s,"instance_nonce":"n1"}\n' "$$" > "$cmd/$instance/process.json"
printf '{"schema_version":1,"instance":"%s","instance_nonce":"n1","finalized":true,"exit_status":0,"payloads":[]}\n' "$instance" > "$cmd/$instance/artifact-manifest.json"
`
	if err := os.WriteFile(binPath, []byte(script), 0755); err != nil {
		t.Fatal(err)
	}
	cfgPath := filepath.Join(absRunDir, "run-config.json")
	data, err := json.Marshal(ctx.RunConfig)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(cfgPath, data, 0644); err != nil {
		t.Fatal(err)
	}

	marker := filepath.Join(absRunDir, "keep-me.txt")
	if err := os.WriteFile(marker, []byte("x"), 0644); err != nil {
		t.Fatal(err)
	}

	if err := o.Cleanup(ctx, true); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(absRunDir); !os.IsNotExist(err) {
		t.Fatalf("remote run dir still present: %v", err)
	}
	if _, err := os.Stat(o.StateStore.RunDir(ctx.RunID)); !os.IsNotExist(err) {
		t.Fatalf("local run state still present: %v", err)
	}
}

func writeCleanupProfile(t *testing.T, dir string) string {
	t.Helper()
	src := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	data, err := os.ReadFile(src)
	if err != nil {
		t.Fatal(err)
	}
	content := string(data)
	content = strings.ReplaceAll(content, "./dist", filepath.Join(dir, "dist"))
	content = strings.ReplaceAll(content, "./remote", filepath.Join(dir, "remote"))
	content = strings.ReplaceAll(content, "./results", filepath.Join(dir, "results"))
	content = strings.ReplaceAll(content, "./state", filepath.Join(dir, "state"))
	path := filepath.Join(dir, "profile.yaml")
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
	return path
}

package orchestrator_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/tpccctl/internal/orchestrator"
	"portable-tpcc/tpccctl/internal/state"
)

func TestPlan_snapshot(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	if err := os.MkdirAll(filepath.Join(dir, "dist"), 0755); err != nil {
		t.Fatal(err)
	}

	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath})
	if err != nil {
		t.Fatal(err)
	}
	plan, err := o.Plan()
	if err != nil {
		t.Fatal(err)
	}
	if plan.RunID == "" {
		t.Fatal("empty run_id")
	}
	if plan.Binary != "tpcc-pgsql" {
		t.Fatalf("binary %q, want tpcc-pgsql", plan.Binary)
	}
	if len(plan.WorkerArgv) != 1 {
		t.Fatalf("worker argv count %d", len(plan.WorkerArgv))
	}
	argv := plan.WorkerArgv["worker-a"]
	if len(argv) != 5 || argv[0] != "worker" {
		t.Fatalf("unexpected argv: %v", argv)
	}
	if plan.WorkerAssignment[0].Threads != 2 {
		t.Fatalf("threads %d, want 2", plan.WorkerAssignment[0].Threads)
	}
}

func TestMaterializePreservesActiveRunState(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-active"})
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
	rs.State = state.StateMeasuring
	if err := o.StateStore.Save(rs); err != nil {
		t.Fatal(err)
	}

	if _, err := o.Materialize(); err != nil {
		t.Fatal(err)
	}
	got, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateMeasuring {
		t.Fatalf("state=%q, want %q", got.State, state.StateMeasuring)
	}
}

func TestMaterializeAutoRunIDSkipsExistingRunDir(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath})
	if err != nil {
		t.Fatal(err)
	}
	first, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	second, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if first.RunID == second.RunID {
		t.Fatalf("auto run IDs should be unique, both were %q", first.RunID)
	}
	if !strings.HasSuffix(first.RunID, "-01") || !strings.HasSuffix(second.RunID, "-02") {
		t.Fatalf("unexpected run IDs: %q, %q", first.RunID, second.RunID)
	}
}

func TestMaterializeRejectsRunIDReuseWithDifferentProfile(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-reuse"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := o.Materialize(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(profilePath)
	if err != nil {
		t.Fatal(err)
	}
	changed := strings.Replace(string(data), "endpoint: localhost:5432", "endpoint: localhost:15432", 1)
	if err := os.WriteFile(profilePath, []byte(changed), 0644); err != nil {
		t.Fatal(err)
	}
	o, err = orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-reuse"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := o.Materialize(); err == nil || !strings.Contains(err.Error(), "different profile") {
		t.Fatalf("expected profile mismatch error, got %v", err)
	}
}

func TestRunAcquiresProfileLockBeforeMaterialize(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-locked"})
	if err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.AcquireProfileLock(o.Profile.Metadata.Name, "other-run"); err != nil {
		t.Fatal(err)
	}
	defer o.StateStore.ReleaseProfileLock(o.Profile.Metadata.Name, "other-run")

	err = o.Run()
	if err == nil || !strings.Contains(err.Error(), "locked by run other-run") {
		t.Fatalf("expected profile lock error, got %v", err)
	}
	if _, err := os.Stat(filepath.Join(o.StateStore.RunDir("run-locked"), "run-config.json")); !os.IsNotExist(err) {
		t.Fatalf("run-config materialized before lock failure: %v", err)
	}
}

func writeTestProfile(t *testing.T, dir, extra string) string {
	t.Helper()
	profileSrc := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	profilePath := filepath.Join(dir, "profile.yaml")
	data, err := os.ReadFile(profileSrc)
	if err != nil {
		t.Fatal(err)
	}
	content := string(data)
	content = replaceAll(content, "./dist", filepath.Join(dir, "dist"))
	content = replaceAll(content, "./remote", filepath.Join(dir, "remote"))
	content = replaceAll(content, "./results", filepath.Join(dir, "results"))
	content = replaceAll(content, "./state", filepath.Join(dir, "state"))
	content += extra
	if err := os.WriteFile(profilePath, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
	return profilePath
}

func replaceAll(s, old, new string) string {
	for {
		i := indexOf(s, old)
		if i < 0 {
			return s
		}
		s = s[:i] + new + s[i+len(old):]
	}
}

func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

package orchestrator_test

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/orchestrator"
	"portable-tpcc/mind/internal/state"
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
	if len(plan.LoadAssignment) != 1 || plan.LoadAssignment[0].Threads != 2 {
		t.Fatalf("loader threads %v, want [2]", plan.LoadAssignment)
	}
	if len(plan.CheckArgvImport) == 0 || plan.CheckArgvImport[len(plan.CheckArgvImport)-1] != "--threads=10" {
		t.Fatalf("check argv %v, want --threads=10", plan.CheckArgvImport)
	}
}

func TestPlanHonorsCheckThreadsOverride(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	if err := os.MkdirAll(filepath.Join(dir, "dist"), 0755); err != nil {
		t.Fatal(err)
	}
	threads := 16
	o, err := orchestrator.New(orchestrator.Options{
		ProfilePath:  profilePath,
		CheckThreads: &threads,
	})
	if err != nil {
		t.Fatal(err)
	}
	plan, err := o.Plan()
	if err != nil {
		t.Fatal(err)
	}
	if got := plan.CheckArgvImport; len(got) == 0 || got[len(got)-1] != "--threads=16" {
		t.Fatalf("check argv %v, want --threads=16", plan.CheckArgvImport)
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

func TestMaterializeAutoRunIDContinuesActiveRun(t *testing.T) {
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
	if first.RunID != second.RunID {
		t.Fatalf("active run should continue, got %q then %q", first.RunID, second.RunID)
	}
}

func TestMaterializeAutoRunIDAllocatesAfterTerminal(t *testing.T) {
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
	if err := o.StateStore.Fail(first.RunID, fmt.Errorf("boom")); err != nil {
		t.Fatal(err)
	}
	second, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if first.RunID == second.RunID {
		t.Fatalf("terminal run should not be reused, both were %q", first.RunID)
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

func TestMaterializeAppliesOverrides(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	warehouses := 3
	rampUp := "10s"
	measurement := "1m"
	o, err := orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		RunID:       "run-override",
		Overrides: config.ProfileOverrides{
			Warehouses:  &warehouses,
			RampUp:      &rampUp,
			Measurement: &measurement,
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if ctx.RunConfig.Scale.Warehouses != 3 {
		t.Fatalf("warehouses=%d, want 3", ctx.RunConfig.Scale.Warehouses)
	}
	if ctx.RunConfig.Phases.RampUpMs != 10000 {
		t.Fatalf("ramp_up_ms=%d, want 10000", ctx.RunConfig.Phases.RampUpMs)
	}
	if ctx.RunConfig.Phases.MeasurementMs != 60000 {
		t.Fatalf("measurement_ms=%d, want 60000", ctx.RunConfig.Phases.MeasurementMs)
	}
}

func TestMaterializeRejectsOverrideConflictOnExistingRun(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	warehouses := 3
	o, err := orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		RunID:       "run-override-conflict",
		Overrides:   config.ProfileOverrides{Warehouses: &warehouses},
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := o.Materialize(); err != nil {
		t.Fatal(err)
	}
	other := 2
	o, err = orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		RunID:       "run-override-conflict",
		Overrides:   config.ProfileOverrides{Warehouses: &other},
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := o.Materialize(); err == nil || !strings.Contains(err.Error(), "conflicts") {
		t.Fatalf("expected override conflict, got %v", err)
	}
}

func TestMaterializeAutoRunIDAllocatesOnOverrideMismatch(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	firstWH := 5
	o, err := orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		Overrides:   config.ProfileOverrides{Warehouses: &firstWH},
	})
	if err != nil {
		t.Fatal(err)
	}
	first, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	// Simulate a finished start stage: non-terminal, but measurement done.
	if err := o.StateStore.Transition(first.RunID, state.StateDraining); err != nil {
		t.Fatal(err)
	}

	secondWH := 3
	o, err = orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		Overrides:   config.ProfileOverrides{Warehouses: &secondWH},
	})
	if err != nil {
		t.Fatal(err)
	}
	second, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if first.RunID == second.RunID {
		t.Fatalf("override mismatch should allocate a new run, both were %q", first.RunID)
	}
	if second.RunConfig.Scale.Warehouses != 3 {
		t.Fatalf("warehouses=%d, want 3", second.RunConfig.Scale.Warehouses)
	}
}

func TestNewRejectsWarehousesIncrease(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	warehouses := 11
	_, err := orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		Overrides:   config.ProfileOverrides{Warehouses: &warehouses},
	})
	if err == nil || !strings.Contains(err.Error(), "exceeds profile") {
		t.Fatalf("expected exceed error, got %v", err)
	}
}

func TestDeployIsProfileScoped(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	dist := filepath.Join(dir, "dist")
	if err := os.MkdirAll(dist, 0755); err != nil {
		t.Fatal(err)
	}
	binPath := filepath.Join(dist, "tpcc-pgsql")
	if err := os.WriteFile(binPath, []byte("#!/bin/sh\n"), 0755); err != nil {
		t.Fatal(err)
	}

	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath})
	if err != nil {
		t.Fatal(err)
	}
	// Pre-create a run and leave it planned; deploy must not advance it.
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	before, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if before.State != state.StatePlanned {
		t.Fatalf("state=%q, want planned", before.State)
	}

	if err := o.Deploy(); err != nil {
		t.Fatal(err)
	}

	after, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State != state.StatePlanned {
		t.Fatalf("deploy mutated run FSM: state=%q, want planned", after.State)
	}
	absRemote, err := filepath.Abs(filepath.Join(dir, "remote"))
	if err != nil {
		t.Fatal(err)
	}
	sharedBin := filepath.Join(absRemote, "tpcc-pgsql")
	if _, err := os.Stat(sharedBin); err != nil {
		t.Fatalf("shared binary missing at %s: %v", sharedBin, err)
	}
	if _, err := os.Stat(filepath.Join(absRemote, ctx.RunID, "tpcc-pgsql")); !os.IsNotExist(err) {
		t.Fatalf("binary should not be under run dir")
	}
}

func TestUndeployRemovesSharedBinary(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	dist := filepath.Join(dir, "dist")
	if err := os.MkdirAll(dist, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dist, "tpcc-pgsql"), []byte("#!/bin/sh\n"), 0755); err != nil {
		t.Fatal(err)
	}

	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath})
	if err != nil {
		t.Fatal(err)
	}
	if err := o.Deploy(); err != nil {
		t.Fatal(err)
	}
	absRemote, err := filepath.Abs(filepath.Join(dir, "remote"))
	if err != nil {
		t.Fatal(err)
	}
	sharedBin := filepath.Join(absRemote, "tpcc-pgsql")
	if _, err := os.Stat(sharedBin); err != nil {
		t.Fatalf("shared binary missing after deploy: %v", err)
	}

	if err := o.Undeploy(false); err == nil {
		t.Fatal("expected error without --yes")
	}
	if err := o.Undeploy(true); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(sharedBin); !os.IsNotExist(err) {
		t.Fatalf("shared binary still present after undeploy: %v", err)
	}
	// Idempotent: binary already gone.
	if err := o.Undeploy(true); err != nil {
		t.Fatalf("second undeploy: %v", err)
	}
}

func TestRequireWorkerBinaryNeedsExplicitDeploy(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	dist := filepath.Join(dir, "dist")
	if err := os.MkdirAll(dist, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dist, "tpcc-pgsql"), []byte("#!/bin/sh\n"), 0755); err != nil {
		t.Fatal(err)
	}

	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath})
	if err != nil {
		t.Fatal(err)
	}
	err = o.Run()
	if err == nil || !strings.Contains(err.Error(), "mind-tpcc deploy") {
		t.Fatalf("expected require-deploy error, got %v", err)
	}

	if err := o.Deploy(); err != nil {
		t.Fatal(err)
	}
	// After explicit deploy, the gate should pass (run will fail later without DB).
	// Call the pipeline gate indirectly by starting Run until past deploy: use
	// Materialize + require by invoking Run with --skip for later stages is
	// unavailable here; Deploy presence is enough that a second check succeeds
	// via openSessions in requireWorkerBinary through a minimal re-check:
	o2, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, SkipSteps: []string{
		"schema", "load", "indexes", "check_after_import", "test",
		"check_after_run", "collect", "consolidate",
	}})
	if err != nil {
		t.Fatal(err)
	}
	if err := o2.Run(); err != nil {
		t.Fatalf("run after deploy with later steps skipped: %v", err)
	}
}

func TestCheckAfterImportRequiresIndexes(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-check-early"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateLoading); err != nil {
		t.Fatal(err)
	}
	err = o.RunCheck(ctx, "after-import")
	if err == nil || !strings.Contains(err.Error(), "requires the indexes stage") {
		t.Fatalf("expected indexes-required error, got %v", err)
	}
	got, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateLoading {
		t.Fatalf("state=%q, want loading (must not enter checking_import)", got.State)
	}
}

func TestCheckPreservesStateOnLaunchFailure(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-check-preserve"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateConsolidating); err != nil {
		t.Fatal(err)
	}
	err = o.RunCheck(ctx, "after-run")
	if err == nil {
		t.Fatal("expected check to fail without a deployed worker binary")
	}
	if strings.Contains(err.Error(), "requires the test stage") {
		t.Fatalf("consolidating should satisfy after-run prerequisites; got %v", err)
	}
	if strings.Contains(err.Error(), "invalid state transition") {
		t.Fatalf("check must not Transition run-state; got %v", err)
	}
	got, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateConsolidating {
		t.Fatalf("state=%q, want consolidating (check must not change run-state)", got.State)
	}
}

func TestCheckAfterImportPreservesIndexingOnLaunchFailure(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-check-indexing"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateIndexing); err != nil {
		t.Fatal(err)
	}
	err = o.RunCheck(ctx, "after-import")
	if err == nil {
		t.Fatal("expected check to fail without a deployed worker binary")
	}
	if strings.Contains(err.Error(), "requires the indexes stage") {
		t.Fatalf("indexes already completed; got %v", err)
	}
	got, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateIndexing {
		t.Fatalf("state=%q, want indexing after failed check", got.State)
	}
}

func TestIndexesAllowedAfterStuckCheckingImport(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-unstick"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateCheckingImport); err != nil {
		t.Fatal(err)
	}
	err = o.RunIndexes(ctx)
	if err == nil {
		t.Fatal("expected indexes to fail without a deployed worker binary")
	}
	if strings.Contains(err.Error(), "invalid state transition") {
		t.Fatalf("indexes must be allowed from checking_import, got %v", err)
	}
	got, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateIndexing {
		t.Fatalf("state=%q, want indexing", got.State)
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

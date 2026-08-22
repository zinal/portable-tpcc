package orchestrator_test

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/mind/internal/canonical"
	"portable-tpcc/mind/internal/collect"
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
	argv := plan.WorkerArgv["h-127-0-0-1-2"]
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

func TestPlanHonorsThreadsOverride(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	if err := os.MkdirAll(filepath.Join(dir, "dist"), 0755); err != nil {
		t.Fatal(err)
	}
	threads := 16
	o, err := orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		Threads:     &threads,
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
	argv := plan.WorkerArgv["h-127-0-0-1-2"]
	if len(argv) == 0 || argv[len(argv)-1] != "--threads=16" {
		t.Fatalf("worker argv %v, want --threads=16", argv)
	}
	loaderArgv := plan.LoaderArgv["h-127-0-0-1-1"]
	if len(loaderArgv) == 0 || loaderArgv[len(loaderArgv)-1] != "--threads=16" {
		t.Fatalf("loader argv %v, want --threads=16", loaderArgv)
	}
	if plan.WorkerAssignment[0].Threads != 2 {
		t.Fatalf("run-config worker threads %d, want profile value 2 (override is argv-only)", plan.WorkerAssignment[0].Threads)
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

func TestMaterializeAllowsMismatchedProfileForCheck(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-check-mismatch"})
	if err != nil {
		t.Fatal(err)
	}
	first, err := o.Materialize()
	if err != nil {
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
	o, err = orchestrator.New(orchestrator.Options{
		ProfilePath:            profilePath,
		RunID:                  "run-check-mismatch",
		AllowMismatchedProfile: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatalf("check should attach to an existing run with a different profile: %v", err)
	}
	if ctx.RunID != first.RunID {
		t.Fatalf("run_id=%q, want %q", ctx.RunID, first.RunID)
	}
	if ctx.RunConfig.Database.Endpoint != first.RunConfig.Database.Endpoint {
		t.Fatalf("run-config endpoint rewritten to %q, want stored %q", ctx.RunConfig.Database.Endpoint, first.RunConfig.Database.Endpoint)
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
		"check_after_test", "collect", "consolidate",
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
	if err == nil || !strings.Contains(err.Error(), "requires a successful data load") {
		t.Fatalf("expected load-required error, got %v", err)
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
	err = o.RunCheck(ctx, "after-test")
	if err == nil {
		t.Fatal("expected check to fail without a deployed worker binary")
	}
	if strings.Contains(err.Error(), "requires a successful data load") {
		t.Fatalf("consolidating should satisfy check prerequisites; got %v", err)
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

func TestCheckAfterTestAllowedAfterIndexing(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-check-after-test-early"})
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
	err = o.RunCheck(ctx, "after-test")
	if err == nil {
		t.Fatal("expected check to fail without a deployed worker binary")
	}
	if strings.Contains(err.Error(), "requires a successful data load") {
		t.Fatalf("after-test should be allowed after load; got %v", err)
	}
	got, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateIndexing {
		t.Fatalf("state=%q, want indexing (check must not change run-state)", got.State)
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
	if strings.Contains(err.Error(), "requires a successful data load") {
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

func TestRunConsolidateCollectsWhenManifestMissing(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-auto-collect"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateDraining); err != nil {
		t.Fatal(err)
	}
	if collect.HasCollectionManifest(o.Expanded.ResultRoot, ctx.RunID) {
		t.Fatal("collection-manifest must be absent before consolidate")
	}

	sha, err := canonical.SHA256File(filepath.Join(ctx.RunDir, "run-config.json"))
	if err != nil {
		t.Fatal(err)
	}
	remoteRoot, err := filepath.Abs(o.Expanded.RemoteRoot)
	if err != nil {
		t.Fatal(err)
	}
	w := ctx.RunConfig.WorkerAssignment[0]
	writeWorkerPayloads(t, filepath.Join(remoteRoot, ctx.RunID, "worker", w.Instance), w, ctx.RunID, sha, "nonce-"+w.Instance)

	if err := o.RunConsolidate(ctx); err != nil {
		t.Fatalf("RunConsolidate: %v", err)
	}
	if !collect.HasCollectionManifest(o.Expanded.ResultRoot, ctx.RunID) {
		t.Fatal("expected collect to write collection-manifest.json")
	}
	if _, err := os.Stat(filepath.Join(o.Expanded.ResultRoot, ctx.RunID, "aggregate.json")); err != nil {
		t.Fatalf("expected aggregate.json after auto-collect: %v", err)
	}
	got, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateConsolidating {
		t.Fatalf("state=%q, want consolidating", got.State)
	}
}

func TestRunConsolidateSkipsCollectWhenManifestPresent(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-already-collected"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateCollecting); err != nil {
		t.Fatal(err)
	}

	sha, err := canonical.SHA256File(filepath.Join(ctx.RunDir, "run-config.json"))
	if err != nil {
		t.Fatal(err)
	}
	w := ctx.RunConfig.WorkerAssignment[0]
	writeWorkerPayloads(t, filepath.Join(o.Expanded.ResultRoot, ctx.RunID, "raw", "worker", w.Instance), w, ctx.RunID, sha, "nonce-"+w.Instance)
	if err := os.WriteFile(collect.CollectionManifestPath(o.Expanded.ResultRoot, ctx.RunID), []byte("{}\n"), 0644); err != nil {
		t.Fatal(err)
	}

	if err := o.RunConsolidate(ctx); err != nil {
		t.Fatalf("RunConsolidate with existing collection must not call collect: %v", err)
	}
	if _, err := os.Stat(filepath.Join(o.Expanded.ResultRoot, ctx.RunID, "aggregate.json")); err != nil {
		t.Fatalf("expected aggregate.json: %v", err)
	}
}

func writeWorkerPayloads(t *testing.T, dir string, w config.WorkerAssignmentJSON, runID, sha, nonce string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	files := map[string]interface{}{
		"result.json": map[string]interface{}{
			"run_id":            runID,
			"instance":          w.Instance,
			"instance_nonce":    nonce,
			"run_config_sha256": sha,
			"assignment": map[string]interface{}{
				"instance":         w.Instance,
				"host":             w.Host,
				"warehouse_ranges": w.WarehouseRanges,
				"threads":          w.Threads,
				"max_inflight":     w.MaxInflight,
			},
			"exit_status": 0,
			"counters":    map[string]interface{}{"new_order_ok": 10},
			"histograms": map[string]interface{}{
				"new_order": map[string]interface{}{
					"layout":       "linear_exp",
					"unit":         "ms",
					"hdr_till":     4,
					"max_value":    64,
					"total_count":  10,
					"min_recorded": 0,
					"max_recorded": 0,
					"sum_values":   0,
					"buckets":      []uint64{10, 0, 0, 0, 0, 0, 0, 0, 0},
				},
			},
		},
		"ready.json": map[string]interface{}{
			"instance_nonce": nonce,
			"clock_calibration": map[string]interface{}{
				"measured_at":    "2026-07-28T12:00:00Z",
				"offset_ms":      5.0,
				"uncertainty_ms": 2.0,
				"rtt_ms":         4.0,
			},
		},
		"process.json": map[string]interface{}{
			"instance_nonce": nonce,
			"run_id":         runID,
			"instance":       w.Instance,
			"role":           "worker",
			"pid":            1234,
		},
	}
	var payloads []collect.ArtifactPayloadEntry
	for name, v := range files {
		data, err := json.MarshalIndent(v, "", "  ")
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, name), data, 0644); err != nil {
			t.Fatal(err)
		}
		sum := sha256.Sum256(data)
		payloads = append(payloads, collect.ArtifactPayloadEntry{
			Path:   name,
			Size:   int64(len(data)),
			SHA256: hex.EncodeToString(sum[:]),
		})
	}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      w.Instance,
		InstanceNonce: nonce,
		Finalized:     true,
		ExitStatus:    0,
		Payloads:      payloads,
	}
	data, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "artifact-manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
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

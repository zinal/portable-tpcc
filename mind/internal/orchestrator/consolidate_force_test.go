package orchestrator_test

import (
	"bytes"
	"encoding/json"
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

func TestConsolidateForceAllowsWorkloadEdit(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-force"})
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
	sha, err := canonical.SHA256File(filepath.Join(ctx.RunDir, "run-config.json"))
	if err != nil {
		t.Fatal(err)
	}
	w := ctx.RunConfig.WorkerAssignment[0]
	writeWorkerPayloads(t, filepath.Join(o.Expanded.ResultRoot, ctx.RunID, "raw", "worker", w.Instance), w, ctx.RunID, sha, "nonce-"+w.Instance)
	writeCollectionManifest(t, o.Expanded.ResultRoot, ctx.RunID)
	before := snapshotRunFiles(t, ctx.RunDir)
	stateBefore, err := os.ReadFile(o.StateStore.StatePath(ctx.RunID))
	if err != nil {
		t.Fatal(err)
	}

	editProfile(t, profilePath, "measurement: 30m", "measurement: 45m")
	editProfile(t, profilePath, "warehouses: 10", "warehouses: 4")

	forced, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, Force: true})
	if err != nil {
		t.Fatal(err)
	}
	id, err := forced.ResolveConsolidateRunID()
	if err != nil {
		t.Fatal(err)
	}
	if id != "run-force" {
		t.Fatalf("run_id=%q, want run-force", id)
	}
	loaded, err := forced.LoadConsolidateContext(id)
	if err != nil {
		t.Fatal(err)
	}
	if loaded.RunConfig.Scale.Warehouses != 10 {
		t.Fatalf("warehouses=%d, want recorded 10", loaded.RunConfig.Scale.Warehouses)
	}
	if loaded.RunConfig.Phases.MeasurementMs != 30*60*1000 {
		t.Fatalf("measurement_ms=%d, want recorded 30m", loaded.RunConfig.Phases.MeasurementMs)
	}
	if err := forced.RunConsolidate(loaded); err != nil {
		t.Fatalf("RunConsolidate: %v", err)
	}
	if !bytes.Equal(before.runConfig, mustRead(t, filepath.Join(ctx.RunDir, "run-config.json"))) ||
		!bytes.Equal(before.profileSHA, mustRead(t, filepath.Join(ctx.RunDir, "profile.sha256"))) ||
		!bytes.Equal(before.redacted, mustRead(t, filepath.Join(ctx.RunDir, "profile.redacted.yaml"))) {
		t.Fatal("consolidate --force rewrote recorded run files")
	}
	afterState, err := os.ReadFile(o.StateStore.StatePath(ctx.RunID))
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Equal(stateBefore, afterState) {
		t.Fatal("expected run state to advance to consolidating")
	}
	got, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateConsolidating {
		t.Fatalf("state=%q, want consolidating", got.State)
	}
	if got.InsecureHostKey {
		t.Fatal("force path recorded insecure_ignore_host_key from the edited profile")
	}

	aggData, err := os.ReadFile(filepath.Join(o.Expanded.ResultRoot, ctx.RunID, "aggregate.json"))
	if err != nil {
		t.Fatal(err)
	}
	var agg struct {
		Settings struct {
			Scale struct {
				Warehouses int `json:"warehouses"`
			} `json:"scale"`
			Phases struct {
				MeasurementMs int64 `json:"measurement_ms"`
			} `json:"phases"`
		} `json:"settings"`
	}
	if err := json.Unmarshal(aggData, &agg); err != nil {
		t.Fatal(err)
	}
	if agg.Settings.Scale.Warehouses != 10 || agg.Settings.Phases.MeasurementMs != 30*60*1000 {
		t.Fatalf("aggregate used edited profile: warehouses=%d measurement_ms=%d",
			agg.Settings.Scale.Warehouses, agg.Settings.Phases.MeasurementMs)
	}
}

func TestConsolidateForceCollectsAfterProfileEdit(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-force-collect"})
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
	beforeCfg := mustRead(t, filepath.Join(ctx.RunDir, "run-config.json"))

	editProfile(t, profilePath, "measurement: 30m", "measurement: 10m")
	forced, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: ctx.RunID, Force: true})
	if err != nil {
		t.Fatal(err)
	}
	id, err := forced.ResolveConsolidateRunID()
	if err != nil {
		t.Fatal(err)
	}
	loaded, err := forced.LoadConsolidateContext(id)
	if err != nil {
		t.Fatal(err)
	}
	if err := forced.RunConsolidate(loaded); err != nil {
		t.Fatalf("RunConsolidate: %v", err)
	}
	if !bytes.Equal(beforeCfg, mustRead(t, filepath.Join(ctx.RunDir, "run-config.json"))) {
		t.Fatal("run-config.json changed")
	}
	if _, err := os.Stat(filepath.Join(o.Expanded.ResultRoot, ctx.RunID, "aggregate.json")); err != nil {
		t.Fatalf("aggregate.json: %v", err)
	}
}

func TestConsolidateWithoutForceRejectsProfileEdit(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-noforce"})
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
	editProfile(t, profilePath, "measurement: 30m", "measurement: 45m")

	plain, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: ctx.RunID})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := plain.Materialize(); err == nil || !strings.Contains(err.Error(), "different profile") {
		t.Fatalf("Materialize err=%v, want different profile", err)
	}
	auto, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath})
	if err != nil {
		t.Fatal(err)
	}
	id, err := auto.ResolveConsolidateRunID()
	if err == nil || id != "" || !strings.Contains(err.Error(), "refusing to allocate") {
		t.Fatalf("id=%q err=%v, want empty id", id, err)
	}
}

func TestConsolidateForceRejectsIdentityChanges(t *testing.T) {
	cases := []struct {
		name    string
		old     string
		new     string
		want    string
		collect bool
	}{
		{name: "dbms", old: "dbms: pgsql", new: "dbms: ydb", want: "database.dbms"},
		{name: "workers", old: "workers:\n  - 127.0.0.1", new: "workers:\n  - 10.1.1.1", want: "workers hosts"},
		{name: "password", old: "password_env: TPCC_PASSWORD", new: "password_env: OTHER_PASSWORD", want: "password_env"},
		{name: "name", old: "name: test-profile", new: "name: other-profile", want: "profile name"},
		{name: "result_root", old: "", new: "", want: "paths.result_root"},
		{name: "ssh", old: "user: tpcc", new: "user: other", want: "ssh.user", collect: true},
		{name: "remote_root", old: "", new: "", want: "paths.remote_root", collect: true},
		{name: "loaders", old: "loaders:\n  - 127.0.0.1", new: "loaders:\n  - 10.2.2.2", want: "loaders hosts", collect: true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			profilePath := writeTestProfile(t, dir, "")
			o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-id-" + tc.name})
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
			if !tc.collect {
				writeCollectionManifest(t, o.Expanded.ResultRoot, ctx.RunID)
			}
			before, err := os.ReadFile(o.StateStore.StatePath(ctx.RunID))
			if err != nil {
				t.Fatal(err)
			}
			switch tc.name {
			case "result_root":
				editProfile(t, profilePath, "result_root: "+o.Expanded.ResultRoot, "result_root: "+filepath.Join(dir, "other-results"))
			case "remote_root":
				editProfile(t, profilePath, "remote_root: "+o.Expanded.RemoteRoot, "remote_root: "+filepath.Join(dir, "other-remote"))
			default:
				editProfile(t, profilePath, tc.old, tc.new)
			}
			forced, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: ctx.RunID, Force: true})
			if err != nil {
				t.Fatal(err)
			}
			id, err := forced.ResolveConsolidateRunID()
			if err == nil || id != "" || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("id=%q err=%v, want %q", id, err, tc.want)
			}
			after, err := os.ReadFile(o.StateStore.StatePath(ctx.RunID))
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(before, after) {
				t.Fatal("state changed after refused consolidate --force")
			}
		})
	}
}

func TestConsolidateForceAllowsNonIdentityEditsWhenCollected(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-endpoint"})
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
	writeCollectionManifest(t, o.Expanded.ResultRoot, ctx.RunID)
	editProfile(t, profilePath, "endpoint: localhost:5432", "endpoint: localhost:15432")
	editProfile(t, profilePath, "user: tpcc", "user: other")
	editProfile(t, profilePath, "loaders:\n  - 127.0.0.1", "loaders:\n  - 10.9.9.9")
	forced, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: ctx.RunID, Force: true})
	if err != nil {
		t.Fatal(err)
	}
	id, err := forced.ResolveConsolidateRunID()
	if err != nil {
		t.Fatalf("collected run should ignore endpoint, ssh, and loaders: %v", err)
	}
	if id != ctx.RunID {
		t.Fatalf("run_id=%q", id)
	}
}

func TestConsolidateForceSkipsInvalidWorkload(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-invalid"})
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
	writeCollectionManifest(t, o.Expanded.ResultRoot, ctx.RunID)
	editProfile(t, profilePath, "warehouses: 10", "warehouses: 0")

	plain, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: ctx.RunID})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := plain.ResolveConsolidateRunID(); err == nil || !strings.Contains(err.Error(), "profile invalid") {
		t.Fatalf("default err=%v, want profile invalid", err)
	}
	forced, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: ctx.RunID, Force: true})
	if err != nil {
		t.Fatal(err)
	}
	id, err := forced.ResolveConsolidateRunID()
	if err != nil {
		t.Fatal(err)
	}
	if id != ctx.RunID {
		t.Fatalf("run_id=%q", id)
	}
}

func TestConsolidateForceBeforeTestLeavesState(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-early"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := o.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(o.StateStore.StatePath(ctx.RunID))
	if err != nil {
		t.Fatal(err)
	}
	editProfile(t, profilePath, "measurement: 30m", "measurement: 45m")
	forced, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: ctx.RunID, Force: true})
	if err != nil {
		t.Fatal(err)
	}
	id, err := forced.ResolveConsolidateRunID()
	if err == nil || id != "" || !strings.Contains(err.Error(), "has not finished test") {
		t.Fatalf("id=%q err=%v", id, err)
	}
	after, err := os.ReadFile(o.StateStore.StatePath(ctx.RunID))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(before, after) {
		t.Fatal("state changed")
	}
}

func TestConsolidateForceRejectsWorkloadOverrides(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	warehouses := 4
	o, err := orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		Force:       true,
		Overrides:   config.ProfileOverrides{Warehouses: &warehouses},
	})
	if err != nil {
		t.Fatal(err)
	}
	id, err := o.ResolveConsolidateRunID()
	if err == nil || id != "" || !strings.Contains(err.Error(), "does not accept --warehouses") {
		t.Fatalf("id=%q err=%v", id, err)
	}
	if _, err := os.Stat(filepath.Join(dir, "state", "runs")); !os.IsNotExist(err) {
		t.Fatalf("override rejection allocated a run: %v", err)
	}
}

func TestConsolidateForceInsecureOverrideWhenCollectNeeded(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-ssh-flag"})
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
	insecure := true
	forced, err := orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		RunID:       ctx.RunID,
		Force:       true,
		Overrides:   config.ProfileOverrides{InsecureIgnoreHostKey: &insecure},
	})
	if err != nil {
		t.Fatal(err)
	}
	id, err := forced.ResolveConsolidateRunID()
	if err == nil || !strings.Contains(err.Error(), "insecure_ignore_host_key") {
		t.Fatalf("id=%q err=%v", id, err)
	}
}

func TestConsolidateForceTerminalDoesNotHideExplicitRun(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeTestProfile(t, dir, "")
	first, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-older"})
	if err != nil {
		t.Fatal(err)
	}
	older, err := first.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if err := first.StateStore.Transition(older.RunID, state.StateDraining); err != nil {
		t.Fatal(err)
	}
	second, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-newer"})
	if err != nil {
		t.Fatal(err)
	}
	newer, err := second.Materialize()
	if err != nil {
		t.Fatal(err)
	}
	if err := second.StateStore.Fail(newer.RunID, os.ErrClosed); err != nil {
		t.Fatal(err)
	}
	editProfile(t, profilePath, "measurement: 30m", "measurement: 45m")

	auto, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, Force: true})
	if err != nil {
		t.Fatal(err)
	}
	id, err := auto.ResolveConsolidateRunID()
	if err == nil || id != "" || !strings.Contains(err.Error(), "refusing to allocate") {
		t.Fatalf("newest terminal run id=%q err=%v", id, err)
	}
	explicit, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: older.RunID, Force: true})
	if err != nil {
		t.Fatal(err)
	}
	id, err = explicit.ResolveConsolidateRunID()
	if err != nil {
		t.Fatal(err)
	}
	if id != older.RunID {
		t.Fatalf("run_id=%q, want %s", id, older.RunID)
	}
}

type recordedRunFiles struct {
	runConfig  []byte
	profileSHA []byte
	redacted   []byte
}

func snapshotRunFiles(t *testing.T, runDir string) recordedRunFiles {
	t.Helper()
	return recordedRunFiles{
		runConfig:  mustRead(t, filepath.Join(runDir, "run-config.json")),
		profileSHA: mustRead(t, filepath.Join(runDir, "profile.sha256")),
		redacted:   mustRead(t, filepath.Join(runDir, "profile.redacted.yaml")),
	}
}

func mustRead(t *testing.T, path string) []byte {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func writeCollectionManifest(t *testing.T, resultRoot, runID string) {
	t.Helper()
	path := collect.CollectionManifestPath(resultRoot, runID)
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("{}\n"), 0644); err != nil {
		t.Fatal(err)
	}
}

func editProfile(t *testing.T, path, old, new string) {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), old) {
		t.Fatalf("profile missing %q", old)
	}
	updated := strings.Replace(string(data), old, new, 1)
	if err := os.WriteFile(path, []byte(updated), 0644); err != nil {
		t.Fatal(err)
	}
}

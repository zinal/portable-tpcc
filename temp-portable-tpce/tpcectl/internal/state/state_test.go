package state_test

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/state"
)

func TestStoreSaveAndLoadRunState(t *testing.T) {
	dir := t.TempDir()
	store := state.NewStore(dir)

	r := &config.ResolvedProfile{
		Profile:        config.Profile{Name: "lab"},
		ProfilePath:    filepath.Join(dir, "profile.yaml"),
		EffectiveRunID: "run-abc",
	}
	if err := os.WriteFile(r.ProfilePath, []byte("profile"), 0600); err != nil {
		t.Fatal(err)
	}

	st, err := state.NewRunState(r, "deadbeef", 123)
	if err != nil {
		t.Fatalf("new run state: %v", err)
	}
	if err := store.SaveRunState(st); err != nil {
		t.Fatalf("save: %v", err)
	}
	loaded, err := store.LoadRunState("run-abc")
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if loaded.RunConfigSHA256 != "deadbeef" || loaded.State != state.PhaseStarting {
		t.Fatalf("unexpected state: %+v", loaded)
	}
}

package collect

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/state"
)

func TestOutputRel(t *testing.T) {
	rel, err := remote.OutputRel("/opt/tpce", "/opt/tpce/runs/run1/bh1")
	if err != nil {
		t.Fatal(err)
	}
	if rel != "runs/run1/bh1" {
		t.Fatalf("rel = %q", rel)
	}
}

func TestCollectDownloadsOutputs(t *testing.T) {
	root := t.TempDir()
	remoteRoot := filepath.Join(root, "remote")
	if err := os.MkdirAll(filepath.Join(remoteRoot, "runs/run1/bh1"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(remoteRoot, "runs/run1/bh1/stdout.log"), []byte("ok\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(remoteRoot, "runs/run1"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(remoteRoot, "runs/run1/run-config.json"), []byte("{}\n"), 0644); err != nil {
		t.Fatal(err)
	}

	profilePath := filepath.Join(root, "profile.yaml")
	if err := os.WriteFile(profilePath, []byte("name: test\n"), 0644); err != nil {
		t.Fatal(err)
	}

	profile := &config.ResolvedProfile{
		Profile: config.Profile{
			Name: "test",
			Paths: config.PathsConfig{
				RemoteRoot: remoteRoot,
			},
			Collect: config.CollectConfig{
				Dest: filepath.Join(root, "results"),
			},
		},
		ProfilePath:    profilePath,
		EffectiveRunID: "run1",
	}

	store := state.NewStore(filepath.Join(root, "state"))
	st := &state.RunState{
		RunID: "run1",
		State: state.PhaseCompleted,
		Processes: []state.ProcessRecord{
			{
				Role: "bh", Name: "bh1", Host: "mid1",
				Output: filepath.Join(remoteRoot, "runs/run1/bh1"),
			},
		},
	}
	if err := store.SaveRunState(st); err != nil {
		t.Fatal(err)
	}
	if err := store.SetCurrentRun(state.ProfileID(profile), "run1"); err != nil {
		t.Fatal(err)
	}

	dial := func(hostName string, p *config.ResolvedProfile) (remote.Session, error) {
		return &remote.LocalSession{Host: hostName, Root: p.Paths.RemoteRoot}, nil
	}
	if err := Run(context.Background(), profile, store, Options{}, dial); err != nil {
		t.Fatalf("collect: %v", err)
	}
	data, err := os.ReadFile(filepath.Join(profile.Collect.Dest, "bh", "bh1", "stdout.log"))
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "ok\n" {
		t.Fatalf("stdout = %q", data)
	}
}

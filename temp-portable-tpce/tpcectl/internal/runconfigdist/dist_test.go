package runconfigdist_test

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/runconfigdist"
)

func TestDistributeLocalHost(t *testing.T) {
	root := t.TempDir()
	hostRoot := filepath.Join(root, "mid1")
	profile := &config.ResolvedProfile{
		Profile: config.Profile{
			Name:  "dist-test",
			Paths: config.PathsConfig{RemoteRoot: hostRoot},
			BH:    []config.BHInstance{{Name: "bh1", Host: "mid1", Listen: 30000, Output: "runs/x/bh1"}},
			MEE:   []config.MEEInstance{{Name: "mee1", Host: "mid1", Listen: 30010, UniqueID: 1, Output: "runs/x/mee1"}},
			StandaloneDriver: &config.StandaloneDriverConfig{
				Enabled: true, Host: "mid1", Users: 1, CEIDBase: 1, DurationSec: 10, Output: "runs/x/drv",
			},
		},
		EffectiveRunID: "run-dist",
		HostAddresses:  map[string]string{"mid1": "127.0.0.1"},
	}
	raw := []byte(`{"schema_version":1,"run_id":"run-dist"}` + "\n")
	sum := sha256Hex(raw)
	dial := func(host string, _ *config.ResolvedProfile) (remote.Executor, error) {
		return &remote.LocalSession{Host: host, Root: hostRoot}, nil
	}
	if err := runconfigdist.Distribute(context.Background(), profile, raw, sum, dial, 0); err != nil {
		t.Fatalf("distribute: %v", err)
	}
	got, err := os.ReadFile(filepath.Join(hostRoot, "runs", "run-dist", "run-config.json"))
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(got) != string(raw) {
		t.Fatalf("content mismatch")
	}
}

func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

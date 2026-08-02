package deploy_test

import (
	"os"
	"path/filepath"
	"testing"

	"portable-tpcc/tpccctl/internal/deploy"
)

func TestDeploy_andCleanup_manifestSafe(t *testing.T) {
	root := t.TempDir()
	src := filepath.Join(root, "src")
	target := filepath.Join(root, "target")
	if err := os.MkdirAll(src, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(src, "bin"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(src, "bin", "tpcc-pgsql"), []byte("binary"), 0644); err != nil {
		t.Fatal(err)
	}
	ld := &deploy.LocalDeploy{SourceRoot: src, TargetRoot: target}
	manifest, err := ld.Deploy(src, true)
	if err != nil {
		t.Fatal(err)
	}
	if !manifest.Complete {
		t.Fatal("manifest not complete")
	}
	dst := filepath.Join(target, "bin", "tpcc-pgsql")
	if _, err := os.Stat(dst); err != nil {
		t.Fatal(err)
	}
	if err := deploy.Cleanup(target, true); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(dst); !os.IsNotExist(err) {
		t.Fatalf("file still exists after cleanup: %v", err)
	}
}

func TestCleanup_requiresYes(t *testing.T) {
	err := deploy.Cleanup("/tmp", false)
	if err == nil {
		t.Fatal("expected error without --yes")
	}
}

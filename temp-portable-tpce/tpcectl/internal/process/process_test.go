package process_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/argv"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/process"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
)

func TestWaitTradeCleanupMarker(t *testing.T) {
	root := t.TempDir()
	ex := &remote.LocalSession{Host: "local", Root: root}
	stdoutRel := "runs/x/dm0/stdout.log"
	if err := ex.MkdirAll("runs/x/dm0", 0755); err != nil {
		t.Fatal(err)
	}
	if err := ex.WriteBytes(stdoutRel, []byte("boot\n"), 0644); err != nil {
		t.Fatal(err)
	}
	offset := process.StdoutSize(ex, stdoutRel)
	go func() {
		time.Sleep(200 * time.Millisecond)
		_ = ex.WriteBytes(stdoutRel, []byte("boot\n"+process.TradeCleanupMarker+"\n"), 0644)
	}()
	if err := process.WaitTradeCleanup(ex, stdoutRel, offset, 2*time.Second); err != nil {
		t.Fatalf("wait: %v", err)
	}
}

func TestStartFakeBinary(t *testing.T) {
	root := t.TempDir()
	binDir := filepath.Join(root, "bin")
	if err := os.MkdirAll(binDir, 0755); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(binDir, "fake-bh.sh")
	content := `#!/bin/bash
set -euo pipefail
READY=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --ready-file) READY="$2"; shift 2;;
    *) shift;;
  esac
done
: > "$READY"
sleep 600
`
	if err := os.WriteFile(script, []byte(content), 0755); err != nil {
		t.Fatal(err)
	}

	ex := &remote.LocalSession{Host: "mid1", Root: root}
	profile := &config.ResolvedProfile{
		Profile: config.Profile{
			Paths: config.PathsConfig{RemoteRoot: root},
		},
		EffectiveRunID: "run1",
	}

	inst := argv.InstanceArgv{
		Role: "mee", Name: "mee1", Host: "mid1",
		Binary: "bin/fake-bh.sh",
		Args:   []string{"--ready-file", "runs/run1/mee1/.service-ready", "-o", "runs/run1/mee1"},
		Output: "runs/run1/mee1",
	}
	pid, _, err := process.Start(ex, profile, "run1", inst)
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	if pid <= 0 {
		t.Fatalf("invalid pid %d", pid)
	}
	alive, err := process.IsAlive(ex, pid)
	if err != nil || !alive {
		t.Fatalf("process not alive: alive=%v err=%v", alive, err)
	}
	if _, err := ex.ReadBytes("runs/run1/mee1/.service-ready"); err != nil {
		t.Fatalf("ready file missing: %v", err)
	}
	_ = process.Stop(ex, pid, time.Second)
	out, _ := ex.ReadBytes("runs/run1/mee1/tpcectl.pid")
	if !strings.Contains(string(out), "0") && len(out) == 0 {
		t.Fatalf("pid file missing")
	}
}

package remote_test

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"portable-tpcc/tpccctl/internal/remote"
)

func TestLocalSession_uploadStartAlive(t *testing.T) {
	root := t.TempDir()
	sess, err := remote.NewLocal("local", "127.0.0.1", root)
	if err != nil {
		t.Fatal(err)
	}
	defer sess.Close()

	src := filepath.Join(root, "src.sh")
	script := "#!/bin/sh\necho ok > \"$1\"\nsleep 0.2\n"
	if err := os.WriteFile(src, []byte(script), 0755); err != nil {
		t.Fatal(err)
	}
	remoteScript := "bin/src.sh"
	if err := sess.Upload(src, remoteScript); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(root, "run", "done.txt")
	pid, err := sess.StartDetached(
		"run",
		filepath.Join(root, remoteScript),
		[]string{marker},
		nil,
		"run/stdout.log",
		"run/stderr.log",
	)
	if err != nil {
		t.Fatal(err)
	}
	if pid <= 0 {
		t.Fatal("expected pid")
	}
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(marker); err == nil {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatal("script did not create marker")
}

func TestIsLoopback(t *testing.T) {
	if !remote.IsLoopback("127.0.0.1") || !remote.IsLoopback("localhost") {
		t.Fatal("expected loopback")
	}
	if remote.IsLoopback("10.0.0.1") {
		t.Fatal("expected non-loopback")
	}
}

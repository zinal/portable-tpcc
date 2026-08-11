package remote_test

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"portable-tpcc/mind/internal/remote"
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

func TestLocalSession_RemoveAll(t *testing.T) {
	root := t.TempDir()
	sess, err := remote.NewLocal("local", "127.0.0.1", root)
	if err != nil {
		t.Fatal(err)
	}
	defer sess.Close()

	runDir := "run-1"
	nested := filepath.Join(runDir, "worker", "w1", "stdout.log")
	if err := sess.MkdirAll(filepath.Dir(nested)); err != nil {
		t.Fatal(err)
	}
	if err := sess.WriteFile(nested, []byte("log")); err != nil {
		t.Fatal(err)
	}
	secret := filepath.Join(runDir, "db-password")
	if err := sess.WriteFileMode(secret, []byte("s3cret"), 0600); err != nil {
		t.Fatal(err)
	}
	st, err := os.Stat(filepath.Join(root, secret))
	if err != nil {
		t.Fatal(err)
	}
	if st.Mode().Perm() != 0600 {
		t.Fatalf("secret mode=%o, want 0600", st.Mode().Perm())
	}
	if err := sess.RemoveAll(runDir); err != nil {
		t.Fatal(err)
	}
	exists, err := sess.Exists(runDir)
	if err != nil {
		t.Fatal(err)
	}
	if exists {
		t.Fatal("run dir still exists after RemoveAll")
	}
	if err := sess.RemoveAll(runDir); err != nil {
		t.Fatalf("idempotent RemoveAll: %v", err)
	}
}

package remote_test

import (
	"bytes"
	"errors"
	"os"
	"os/exec"
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

func TestLocalSession_uploadReplacesBusyExecutable(t *testing.T) {
	sleepPath, err := exec.LookPath("sleep")
	if err != nil {
		t.Skip("sleep not found on PATH")
	}
	root := t.TempDir()
	sess, err := remote.NewLocal("local", "127.0.0.1", root)
	if err != nil {
		t.Fatal(err)
	}
	defer sess.Close()

	src := filepath.Join(root, "sleep.bin")
	data, err := os.ReadFile(sleepPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(src, data, 0755); err != nil {
		t.Fatal(err)
	}
	remoteBin := "bin/tpcc-busy"
	if err := sess.Upload(src, remoteBin); err != nil {
		t.Fatal(err)
	}
	pid, err := sess.StartDetached(
		"run",
		filepath.Join(root, remoteBin),
		[]string{"30"},
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
	defer sess.Signal(pid, "KILL")

	deadline := time.Now().Add(2 * time.Second)
	for {
		alive, err := sess.IsAlive(pid)
		if err != nil {
			t.Fatal(err)
		}
		if alive {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("process did not start")
		}
		time.Sleep(20 * time.Millisecond)
	}

	replacement := filepath.Join(root, "replacement.bin")
	if err := os.WriteFile(replacement, []byte("#!/bin/sh\necho replaced\n"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := sess.Upload(replacement, remoteBin); err != nil {
		t.Fatalf("upload over busy executable: %v", err)
	}
	alive, err := sess.IsAlive(pid)
	if err != nil {
		t.Fatal(err)
	}
	if !alive {
		t.Fatal("running process should survive inode unlink")
	}
	got, err := os.ReadFile(filepath.Join(root, remoteBin))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "#!/bin/sh\necho replaced\n" {
		t.Fatalf("replaced content = %q", got)
	}
}

func TestLocalSession_uploadDoesNotExposeEmptyDest(t *testing.T) {
	root := t.TempDir()
	sess, err := remote.NewLocal("local", "127.0.0.1", root)
	if err != nil {
		t.Fatal(err)
	}
	defer sess.Close()

	dstRel := "ca.pem"
	dst := filepath.Join(root, dstRel)
	old := []byte("-----BEGIN OLD CERT-----\n")
	if err := os.WriteFile(dst, old, 0644); err != nil {
		t.Fatal(err)
	}
	src := filepath.Join(root, "new.pem")
	newContent := bytes.Repeat([]byte("N"), 1<<20)
	if err := os.WriteFile(src, newContent, 0644); err != nil {
		t.Fatal(err)
	}

	done := make(chan struct{})
	errCh := make(chan error, 1)
	started := make(chan struct{})
	go func() {
		close(started)
		for {
			select {
			case <-done:
				errCh <- nil
				return
			default:
				data, err := os.ReadFile(dst)
				if err != nil {
					continue
				}
				if len(data) == 0 {
					errCh <- errors.New("reader observed empty dest during upload")
					return
				}
			}
		}
	}()
	<-started
	if err := sess.Upload(src, dstRel); err != nil {
		t.Fatal(err)
	}
	close(done)
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(dst)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, newContent) {
		t.Fatalf("dest len=%d, want %d", len(got), len(newContent))
	}
	if _, err := os.Stat(dst + ".tmp"); !os.IsNotExist(err) {
		t.Fatalf("leftover tmp: %v", err)
	}
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

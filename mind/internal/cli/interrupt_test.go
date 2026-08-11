package cli

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"

	"portable-tpcc/mind/internal/orchestrator"
)

func TestWithMaterializedProfileLock_releasesOnInterrupt(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	interrupt, cancel := context.WithCancel(context.Background())

	o, err := orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		RunID:       "run-interrupt",
		Interrupt:   interrupt,
	})
	if err != nil {
		t.Fatal(err)
	}

	started := make(chan struct{})
	errCh := make(chan error, 1)
	go func() {
		errCh <- withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error {
			close(started)
			<-o.Opts.Interrupt.Done()
			return orchestrator.ErrInterrupted
		})
	}()

	select {
	case <-started:
	case <-time.After(2 * time.Second):
		t.Fatal("locked stage did not start")
	}

	lockPath := o.StateStore.ProfileLockPath(o.Profile.Metadata.Name)
	if _, err := os.Stat(lockPath); err != nil {
		t.Fatalf("expected profile lock during stage: %v", err)
	}

	cancel()
	select {
	case err := <-errCh:
		if !errors.Is(err, orchestrator.ErrInterrupted) {
			t.Fatalf("expected ErrInterrupted, got %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("stage did not return after interrupt")
	}

	if _, err := os.Stat(lockPath); !os.IsNotExist(err) {
		t.Fatalf("profile lock should be released after interrupt, stat=%v", err)
	}
}

func TestExitErrInterruptedCode(t *testing.T) {
	stderr := os.Stderr
	devNull, err := os.Open(os.DevNull)
	if err != nil {
		t.Fatal(err)
	}
	defer devNull.Close()
	os.Stderr = devNull
	defer func() { os.Stderr = stderr }()

	if code := exitErr(orchestrator.ErrInterrupted); code != 130 {
		t.Fatalf("exitErr(ErrInterrupted)=%d, want 130", code)
	}
	if code := exitErr(errors.New("boom")); code != 1 {
		t.Fatalf("exitErr(other)=%d, want 1", code)
	}
}

// TestSIGINTReleasesProfileLock sends a real SIGINT to a child that holds the
// profile lock the same way `mind-tpcc load` does (NotifyContext + defer release).
func TestSIGINTReleasesProfileLock(t *testing.T) {
	if os.Getenv("MIND_TPCC_SIGINT_HELPER") == "1" {
		os.Exit(runSIGINTHelperLocked(os.Getenv("MIND_TPCC_PROFILE"), os.Getenv("MIND_TPCC_READY")))
	}

	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	stateDir := filepath.Join(dir, "state")
	lockPath := filepath.Join(stateDir, "profiles", "test-profile", "run.lock")
	readyPath := filepath.Join(dir, "ready")

	cmd := exec.Command(os.Args[0], "-test.run=^TestSIGINTReleasesProfileLock$", "-test.v=false")
	cmd.Env = append(os.Environ(),
		"MIND_TPCC_SIGINT_HELPER=1",
		"MIND_TPCC_PROFILE="+profilePath,
		"MIND_TPCC_READY="+readyPath,
	)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}

	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, err := os.Stat(readyPath); err == nil {
			break
		}
		if time.Now().After(deadline) {
			_ = cmd.Process.Kill()
			t.Fatal("helper did not become ready with profile lock")
		}
		time.Sleep(10 * time.Millisecond)
	}
	if _, err := os.Stat(lockPath); err != nil {
		_ = cmd.Process.Kill()
		t.Fatalf("expected lock before SIGINT: %v", err)
	}

	if err := cmd.Process.Signal(os.Interrupt); err != nil {
		_ = cmd.Process.Kill()
		t.Fatal(err)
	}
	err := cmd.Wait()
	if err == nil {
		t.Fatal("helper exited 0, want non-zero interrupt status")
	}

	if _, err := os.Stat(lockPath); !os.IsNotExist(err) {
		t.Fatalf("profile lock should be released after SIGINT, stat=%v", err)
	}
}

func runSIGINTHelperLocked(profilePath, readyPath string) int {
	// Mirror Run(): NotifyContext so SIGINT cancels instead of hard-killing.
	interrupt, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	o, err := orchestrator.New(orchestrator.Options{
		ProfilePath: profilePath,
		RunID:       "run-sigint",
		Interrupt:   interrupt,
	})
	if err != nil {
		return exitErr(err)
	}
	err = withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error {
		if err := os.WriteFile(readyPath, []byte("1"), 0644); err != nil {
			return err
		}
		<-o.Opts.Interrupt.Done()
		return orchestrator.ErrInterrupted
	})
	if err != nil {
		return exitErr(err)
	}
	return 0
}

func writeCLITestProfile(t *testing.T, dir string) string {
	t.Helper()
	profileSrc := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	profilePath := filepath.Join(dir, "profile.yaml")
	data, err := os.ReadFile(profileSrc)
	if err != nil {
		t.Fatal(err)
	}
	content := string(data)
	content = strings.ReplaceAll(content, "./dist", filepath.Join(dir, "dist"))
	content = strings.ReplaceAll(content, "./remote", filepath.Join(dir, "remote"))
	content = strings.ReplaceAll(content, "./results", filepath.Join(dir, "results"))
	content = strings.ReplaceAll(content, "./state", filepath.Join(dir, "state"))
	if err := os.MkdirAll(filepath.Join(dir, "dist"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(profilePath, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
	return profilePath
}

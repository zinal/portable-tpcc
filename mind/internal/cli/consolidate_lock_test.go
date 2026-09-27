package cli

import (
	"errors"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"portable-tpcc/mind/internal/orchestrator"
	"portable-tpcc/mind/internal/state"
)

func TestConsolidateProceedsWhileOtherRunHoldsProfileLock(t *testing.T) {
	o := prepareDrainedRun(t, "run-old")
	const holder = "ydb2400h_run01"
	if err := o.StateStore.AcquireProfileLock(o.Profile.Metadata.Name, holder); err != nil {
		t.Fatal(err)
	}
	defer o.StateStore.ReleaseProfileLock(o.Profile.Metadata.Name, holder)

	sentinel := errors.New("consolidate body")
	called := false
	err := withConsolidateLock(o, func(ctx *orchestrator.Context) error {
		called = true
		if ctx.RunID != "run-old" {
			t.Errorf("run id %q, want run-old", ctx.RunID)
		}
		return sentinel
	})
	if !called {
		t.Fatalf("consolidate did not start: %v", err)
	}
	if !errors.Is(err, sentinel) {
		t.Fatalf("got %v", err)
	}

	data, err := os.ReadFile(o.StateStore.ProfileLockPath(o.Profile.Metadata.Name))
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != holder {
		t.Fatalf("profile lock owner %q, want %s", data, holder)
	}
	if err := o.StateStore.AcquireRunLock("run-old"); err != nil {
		t.Fatalf("run lock not released: %v", err)
	}
	if err := o.StateStore.ReleaseRunLock("run-old"); err != nil {
		t.Fatal(err)
	}
}

func TestConsolidateBlockedWhileSameRunLocked(t *testing.T) {
	o := prepareDrainedRun(t, "run-old")
	before, err := os.ReadFile(o.StateStore.StatePath("run-old"))
	if err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.AcquireRunLock("run-old"); err != nil {
		t.Fatal(err)
	}
	defer o.StateStore.ReleaseRunLock("run-old")

	err = withConsolidateLock(o, func(*orchestrator.Context) error {
		t.Fatal("consolidate ran while its run lock was held")
		return nil
	})
	if err == nil || !strings.Contains(err.Error(), "run run-old locked") {
		t.Fatalf("expected run lock error, got %v", err)
	}
	after, err := os.ReadFile(o.StateStore.StatePath("run-old"))
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != string(before) {
		t.Fatalf("state rewritten while run lock held:\n%s", after)
	}
}

func TestTestHoldsRunLockWhileProfileLockHeld(t *testing.T) {
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: "run-live"})
	if err != nil {
		t.Fatal(err)
	}

	started := make(chan struct{})
	release := make(chan struct{})
	var once sync.Once
	stop := func() { once.Do(func() { close(release) }) }
	t.Cleanup(stop)
	errCh := make(chan error, 1)
	go func() {
		errCh <- withMaterializedProfileLock(o, func(*orchestrator.Context) error {
			close(started)
			<-release
			return nil
		})
	}()
	select {
	case <-started:
	case <-time.After(2 * time.Second):
		t.Fatal("stage did not acquire locks")
	}

	err = o.StateStore.AcquireRunLock("run-live")
	if err == nil || !strings.Contains(err.Error(), "run run-live locked") {
		t.Fatalf("expected live test to hold the run lock, got %v", err)
	}
	stop()
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.AcquireRunLock("run-live"); err != nil {
		t.Fatalf("run lock not released: %v", err)
	}
	if err := o.StateStore.ReleaseRunLock("run-live"); err != nil {
		t.Fatal(err)
	}
}

func prepareDrainedRun(t *testing.T, runID string) *orchestrator.Orchestrator {
	t.Helper()
	dir := t.TempDir()
	profilePath := writeCLITestProfile(t, dir)
	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath, RunID: runID})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := o.Materialize(); err != nil {
		t.Fatal(err)
	}
	if err := o.StateStore.Transition(runID, state.StateDraining); err != nil {
		t.Fatal(err)
	}
	return o
}

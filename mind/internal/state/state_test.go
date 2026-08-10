package state_test

import (
	"strings"
	"sync"
	"testing"

	"portable-tpcc/mind/internal/state"
)

func TestAcquireProfileLockConcurrent(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	const attempts = 32

	var wg sync.WaitGroup
	start := make(chan struct{})
	errs := make(chan error, attempts)
	for i := 0; i < attempts; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			<-start
			errs <- store.AcquireProfileLock("profile-a", "run")
		}(i)
	}
	close(start)
	wg.Wait()
	close(errs)

	successes := 0
	for err := range errs {
		if err == nil {
			successes++
		}
	}
	if successes != 1 {
		t.Fatalf("successful lock acquisitions=%d, want 1", successes)
	}
}

func TestTransitionRejectsBackwardMove(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	rs := &state.RunState{
		SchemaVersion: 1,
		RunID:         "run-1",
		State:         state.StateMeasuring,
	}
	if err := store.Save(rs); err != nil {
		t.Fatal(err)
	}

	err := store.Transition("run-1", state.StatePlanned)
	if err == nil || !strings.Contains(err.Error(), "invalid state transition") {
		t.Fatalf("expected invalid transition error, got %v", err)
	}
	got, err := store.Load("run-1")
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateMeasuring {
		t.Fatalf("state=%q, want %q", got.State, state.StateMeasuring)
	}
}

func TestTransitionRejectsTerminalRewrite(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	for _, terminal := range []string{state.StateCompleted, state.StateFailed} {
		rs := &state.RunState{
			SchemaVersion: 1,
			RunID:         "run-" + terminal,
			State:         terminal,
		}
		if err := store.Save(rs); err != nil {
			t.Fatal(err)
		}
		err := store.Transition(rs.RunID, state.StateCollecting)
		if err == nil || !strings.Contains(err.Error(), "invalid state transition") {
			t.Fatalf("expected terminal transition error for %s, got %v", terminal, err)
		}
		got, err := store.Load(rs.RunID)
		if err != nil {
			t.Fatal(err)
		}
		if got.State != terminal {
			t.Fatalf("state=%q, want %q", got.State, terminal)
		}
	}
}

func TestTransitionAllowsForwardAndStopping(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	rs := &state.RunState{
		SchemaVersion: 1,
		RunID:         "run-1",
		State:         state.StateMeasuring,
	}
	if err := store.Save(rs); err != nil {
		t.Fatal(err)
	}
	if err := store.Transition("run-1", state.StateDraining); err != nil {
		t.Fatalf("forward transition failed: %v", err)
	}
	if err := store.Transition("run-1", state.StateStopping); err != nil {
		t.Fatalf("stopping transition failed: %v", err)
	}
	if !state.IsTerminal(state.StateCompleted) || !state.IsTerminal(state.StateFailed) {
		t.Fatalf("completed and failed should be terminal")
	}
}

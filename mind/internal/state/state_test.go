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

func TestTransitionAllowsIndexesAfterPrematureCheck(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	rs := &state.RunState{
		SchemaVersion: 1,
		RunID:         "run-1",
		State:         state.StateCheckingImport,
	}
	if err := store.Save(rs); err != nil {
		t.Fatal(err)
	}
	if err := store.Transition("run-1", state.StateIndexing); err != nil {
		t.Fatalf("recovery transition checking_import -> indexing failed: %v", err)
	}
	got, err := store.Load("run-1")
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateIndexing {
		t.Fatalf("state=%q, want %q", got.State, state.StateIndexing)
	}
}

func TestTransitionAllowsCollectAfterConsolidate(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	rs := &state.RunState{
		SchemaVersion: 1,
		RunID:         "run-1",
		State:         state.StateConsolidating,
	}
	if err := store.Save(rs); err != nil {
		t.Fatal(err)
	}
	if err := store.Transition("run-1", state.StateCollecting); err != nil {
		t.Fatalf("recovery consolidating -> collecting failed: %v", err)
	}
	got, err := store.Load("run-1")
	if err != nil {
		t.Fatal(err)
	}
	if got.State != state.StateCollecting {
		t.Fatalf("state=%q, want %q", got.State, state.StateCollecting)
	}
}

func TestTransitionRecoveryMatrix(t *testing.T) {
	cases := []struct {
		from, to string
		ok       bool
	}{
		{state.StateCheckingImport, state.StateIndexing, true},
		{state.StateCheckingResult, state.StateIndexing, true},
		{state.StateDraining, state.StateIndexing, true},
		{state.StateCollecting, state.StateIndexing, true},
		{state.StateConsolidating, state.StateIndexing, true},
		{state.StateConsolidating, state.StateCollecting, true},
		{state.StateConsolidating, state.StateCheckingResult, false},
		{state.StateCollecting, state.StateCheckingResult, false},
		{state.StateCheckingImport, state.StateLoading, false},
		{state.StateCheckingImport, state.StateSchema, false},
		{state.StateCheckingImport, state.StatePreparing, true},
		{state.StateMeasuring, state.StateIndexing, false},
		{state.StatePreparing, state.StateIndexing, false},
		{state.StateArming, state.StateIndexing, false},
		{state.StateRamping, state.StateIndexing, false},
		{state.StateFailed, state.StateIndexing, false},
		{state.StateCompleted, state.StateIndexing, false},
		{state.StateLoading, state.StateIndexing, true},
		{state.StateIndexing, state.StateCheckingImport, true},
		{state.StateLoading, state.StateCheckingImport, true},
	}
	for _, tc := range cases {
		store := &state.Store{StateDir: t.TempDir()}
		rs := &state.RunState{SchemaVersion: 1, RunID: "run-1", State: tc.from}
		if err := store.Save(rs); err != nil {
			t.Fatal(err)
		}
		err := store.Transition("run-1", tc.to)
		if tc.ok {
			if err != nil {
				t.Fatalf("%s -> %s: unexpected error %v", tc.from, tc.to, err)
			}
			continue
		}
		if err == nil || !strings.Contains(err.Error(), "invalid state transition") {
			t.Fatalf("%s -> %s: expected invalid transition, got %v", tc.from, tc.to, err)
		}
		got, err := store.Load("run-1")
		if err != nil {
			t.Fatal(err)
		}
		if got.State != tc.from {
			t.Fatalf("%s -> %s: state became %q", tc.from, tc.to, got.State)
		}
	}
}

func TestReached(t *testing.T) {
	if !state.Reached(state.StateIndexing, state.StateIndexing) {
		t.Fatal("indexing should reach indexing")
	}
	if !state.Reached(state.StateCheckingImport, state.StateIndexing) {
		t.Fatal("checking_import should reach indexing")
	}
	if state.Reached(state.StateLoading, state.StateIndexing) {
		t.Fatal("loading should not reach indexing")
	}
	if state.Reached(state.StateFailed, state.StateIndexing) {
		t.Fatal("failed is not a pipeline state")
	}
}

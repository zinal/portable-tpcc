package orchestrator

import (
	"strings"
	"testing"

	"portable-tpcc/mind/internal/state"
)

func TestRequireSuccessfulLoadForCheck(t *testing.T) {
	cases := []struct {
		name    string
		state   string
		skipped []string
		wantErr bool
	}{
		{name: "loading", state: state.StateLoading, wantErr: true},
		{name: "schema", state: state.StateSchema, wantErr: true},
		{name: "planned", state: state.StatePlanned, wantErr: true},
		{name: "indexing", state: state.StateIndexing},
		{name: "checking_import", state: state.StateCheckingImport},
		{name: "consolidating", state: state.StateConsolidating},
		{name: "completed", state: state.StateCompleted},
		{name: "skipped_indexes", state: state.StateLoading, skipped: []string{"indexes"}},
		{name: "failed_not_reached", state: state.StateFailed, wantErr: true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rs := &state.RunState{State: tc.state, SkippedSteps: tc.skipped}
			err := requireSuccessfulLoadForCheck(rs, "after-import")
			if tc.wantErr {
				if err == nil || !strings.Contains(err.Error(), "requires a successful data load") {
					t.Fatalf("expected load-required error, got %v", err)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
		})
	}
}

func TestRequireCheckPhase(t *testing.T) {
	cases := []struct {
		name    string
		phase   string
		state   string
		skipped []string
		wantErr string
	}{
		{name: "after_import_ok_indexing", phase: "after-import", state: state.StateIndexing},
		{name: "after_import_ok_consolidating", phase: "after-import", state: state.StateConsolidating},
		{name: "after_import_needs_load", phase: "after-import", state: state.StateLoading, wantErr: "requires a successful data load"},
		{name: "after_test_ok_indexing", phase: "after-test", state: state.StateIndexing},
		{name: "after_test_ok_draining", phase: "after-test", state: state.StateDraining},
		{name: "after_test_ok_consolidating", phase: "after-test", state: state.StateConsolidating},
		{name: "after_test_ok_completed", phase: "after-test", state: state.StateCompleted},
		{name: "after_test_ok_measuring", phase: "after-test", state: state.StateMeasuring},
		{name: "after_test_needs_load", phase: "after-test", state: state.StateLoading, wantErr: "requires a successful data load"},
		{name: "after_test_ok_skipped_indexes", phase: "after-test", state: state.StateLoading, skipped: []string{"indexes"}},
		{name: "refuses_failed", phase: "after-test", state: state.StateFailed, wantErr: "refused while run is failed"},
		{name: "refuses_stopping", phase: "after-import", state: state.StateStopping, wantErr: "refused while run is stopping"},
		{name: "unknown_phase", phase: "mid-run", state: state.StateDraining, wantErr: "unknown check phase"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rs := &state.RunState{State: tc.state, SkippedSteps: tc.skipped}
			err := requireCheckPhase(rs, tc.phase)
			if tc.wantErr == "" {
				if err != nil {
					t.Fatalf("unexpected error: %v", err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("expected error containing %q, got %v", tc.wantErr, err)
			}
		})
	}
}

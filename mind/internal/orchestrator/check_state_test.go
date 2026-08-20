package orchestrator

import (
	"strings"
	"testing"

	"portable-tpcc/mind/internal/state"
)

func TestRequireIndexesForImportCheck(t *testing.T) {
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
		{name: "skipped_indexes", state: state.StateLoading, skipped: []string{"indexes"}},
		{name: "failed_defers_to_transition", state: state.StateFailed},
		{name: "stopping_defers_to_transition", state: state.StateStopping},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rs := &state.RunState{State: tc.state, SkippedSteps: tc.skipped}
			err := requireIndexesForImportCheck(rs)
			if tc.wantErr {
				if err == nil || !strings.Contains(err.Error(), "requires the indexes stage") {
					t.Fatalf("expected indexes-required error, got %v", err)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
		})
	}
}

package consolidate_test

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"portable-tpcc/tools/tpccctl/internal/config"
	"portable-tpcc/tools/tpccctl/internal/consolidate"
)

func TestConsolidate_mergesHistograms(t *testing.T) {
	root := t.TempDir()
	runID := "run-hist"
	workerDir := filepath.Join(root, runID, "raw", "worker", "worker-a")
	if err := os.MkdirAll(workerDir, 0755); err != nil {
		t.Fatal(err)
	}
	result := map[string]interface{}{
		"exit_status": 0,
		"counters": map[string]interface{}{
			"new_order_ok": 100,
		},
		"histograms": map[string]interface{}{
			"new_order": map[string]interface{}{
				"layout":       "linear_exp",
				"unit":         "ms",
				"hdr_till":     4,
				"max_value":    64,
				"total_count":  4,
				"max_recorded": 3,
				"buckets":      []uint64{1, 1, 1, 1, 0, 0, 0},
			},
		},
	}
	data, _ := json.MarshalIndent(result, "", "  ")
	if err := os.WriteFile(filepath.Join(workerDir, "result.json"), data, 0644); err != nil {
		t.Fatal(err)
	}
	checksDir := filepath.Join(root, runID, "checks")
	if err := os.MkdirAll(checksDir, 0755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(checksDir, "after-run.json"), []byte(`{"ok":true}`), 0644)

	rc := &config.RunConfig{
		RunID: runID,
		Phases: config.PhasesJSON{
			MeasurementMs:  60000,
			MaxClockSkewMs: 100,
		},
		Scale: config.ScaleBlock{Warehouses: 10},
		WorkerAssignment: []config.WorkerAssignmentJSON{
			{Instance: "worker-a", WarehouseRanges: [][]int{{1, 11}}},
		},
	}
	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.Consolidate(runID, rc)
	if err != nil {
		t.Fatal(err)
	}
	if !agg.Status.WorkersComplete || !agg.Status.IntegrityOK {
		t.Fatalf("status %+v", agg.Status)
	}
	meas := agg.Metrics["measurement"].(map[string]interface{})
	rt := meas["response_time_ms"].(map[string]interface{})
	if _, ok := rt["new_order"]; !ok {
		t.Fatalf("missing percentiles: %#v", rt)
	}
}

package consolidate_test

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
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
	ready := map[string]interface{}{
		"clock_calibration": map[string]interface{}{
			"measured_at":    "2026-07-28T12:00:00Z",
			"offset_ms":      5.0,
			"uncertainty_ms": 2.0,
			"rtt_ms":         4.0,
		},
	}
	readyData, _ := json.MarshalIndent(ready, "", "  ")
	if err := os.WriteFile(filepath.Join(workerDir, "ready.json"), readyData, 0644); err != nil {
		t.Fatal(err)
	}
	checksDir := filepath.Join(root, runID, "checks")
	if err := os.MkdirAll(checksDir, 0755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(checksDir, "after-import.json"), []byte(`{"ok":true}`), 0644)
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

func TestConsolidate_clockSkewExceeded(t *testing.T) {
	root := t.TempDir()
	runID := "run-skew"
	workerDir := filepath.Join(root, runID, "raw", "worker", "worker-a")
	if err := os.MkdirAll(workerDir, 0755); err != nil {
		t.Fatal(err)
	}
	result := map[string]interface{}{
		"exit_status": 0,
		"counters": map[string]interface{}{
			"new_order_ok": 10,
		},
	}
	resultData, _ := json.MarshalIndent(result, "", "  ")
	if err := os.WriteFile(filepath.Join(workerDir, "result.json"), resultData, 0644); err != nil {
		t.Fatal(err)
	}
	ready := map[string]interface{}{
		"clock_calibration": map[string]interface{}{
			"measured_at":    "2026-07-28T12:00:00Z",
			"offset_ms":      250.0,
			"uncertainty_ms": 2.0,
			"rtt_ms":         4.0,
		},
	}
	readyData, _ := json.MarshalIndent(ready, "", "  ")
	if err := os.WriteFile(filepath.Join(workerDir, "ready.json"), readyData, 0644); err != nil {
		t.Fatal(err)
	}

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
	if agg.Status.ClockSkewOK {
		t.Fatalf("expected clock_skew_ok=false, got %+v", agg.Status)
	}
}

func TestConsolidate_clockSkewSpreadExceeded(t *testing.T) {
	root := t.TempDir()
	runID := "run-spread"
	maxSkew := int64(100)
	for _, spec := range []struct {
		name   string
		offset float64
	}{
		{"worker-a", 5.0},
		{"worker-b", 120.0},
	} {
		workerDir := filepath.Join(root, runID, "raw", "worker", spec.name)
		if err := os.MkdirAll(workerDir, 0755); err != nil {
			t.Fatal(err)
		}
		result := map[string]interface{}{
			"exit_status": 0,
			"counters": map[string]interface{}{
				"new_order_ok": 10,
			},
		}
		resultData, _ := json.MarshalIndent(result, "", "  ")
		if err := os.WriteFile(filepath.Join(workerDir, "result.json"), resultData, 0644); err != nil {
			t.Fatal(err)
		}
		ready := map[string]interface{}{
			"clock_calibration": map[string]interface{}{
				"measured_at":    "2026-07-28T12:00:00Z",
				"offset_ms":      spec.offset,
				"uncertainty_ms": 2.0,
				"rtt_ms":         4.0,
				"time_source":    "db-a",
			},
		}
		readyData, _ := json.MarshalIndent(ready, "", "  ")
		if err := os.WriteFile(filepath.Join(workerDir, "ready.json"), readyData, 0644); err != nil {
			t.Fatal(err)
		}
	}

	rc := &config.RunConfig{
		RunID: runID,
		Phases: config.PhasesJSON{
			MeasurementMs:  60000,
			MaxClockSkewMs: maxSkew,
		},
		Scale: config.ScaleBlock{Warehouses: 20},
		WorkerAssignment: []config.WorkerAssignmentJSON{
			{Instance: "worker-a", WarehouseRanges: [][]int{{1, 11}}},
			{Instance: "worker-b", WarehouseRanges: [][]int{{11, 21}}},
		},
	}
	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.Consolidate(runID, rc)
	if err != nil {
		t.Fatal(err)
	}
	if agg.Status.ClockSkewOK {
		t.Fatalf("expected clock_skew_ok=false for spread=115, got %+v", agg.Status)
	}
}

func writeMinimalWorkerArtifacts(t *testing.T, root, runID, workerName string) {
	t.Helper()
	workerDir := filepath.Join(root, runID, "raw", "worker", workerName)
	if err := os.MkdirAll(workerDir, 0755); err != nil {
		t.Fatal(err)
	}
	result := map[string]interface{}{
		"exit_status": 0,
		"counters": map[string]interface{}{
			"new_order_ok": 10,
		},
	}
	resultData, _ := json.MarshalIndent(result, "", "  ")
	if err := os.WriteFile(filepath.Join(workerDir, "result.json"), resultData, 0644); err != nil {
		t.Fatal(err)
	}
	ready := map[string]interface{}{
		"clock_calibration": map[string]interface{}{
			"measured_at":    "2026-07-28T12:00:00Z",
			"offset_ms":      5.0,
			"uncertainty_ms": 2.0,
			"rtt_ms":         4.0,
		},
	}
	readyData, _ := json.MarshalIndent(ready, "", "  ")
	if err := os.WriteFile(filepath.Join(workerDir, "ready.json"), readyData, 0644); err != nil {
		t.Fatal(err)
	}
}

func minimalRunConfig(runID string) *config.RunConfig {
	return &config.RunConfig{
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
}

func TestConsolidate_integrityMissingChecks(t *testing.T) {
	root := t.TempDir()
	runID := "run-no-checks"
	writeMinimalWorkerArtifacts(t, root, runID, "worker-a")

	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.Consolidate(runID, minimalRunConfig(runID))
	if err != nil {
		t.Fatal(err)
	}
	if agg.Status.IntegrityOK {
		t.Fatalf("expected integrity_ok=false when checks are missing, got %+v", agg.Status)
	}
	if len(agg.Status.IntegrityErrors) == 0 {
		t.Fatalf("expected integrity_errors with missing report details, got %+v", agg.Status)
	}
	joined := strings.Join(agg.Status.IntegrityErrors, "\n")
	if !strings.Contains(joined, "after-import.json") || !strings.Contains(joined, "after-run.json") {
		t.Fatalf("expected missing report names in integrity_errors, got %#v", agg.Status.IntegrityErrors)
	}
}

func TestConsolidate_integrityFailedCheck(t *testing.T) {
	root := t.TempDir()
	runID := "run-check-fail"
	writeMinimalWorkerArtifacts(t, root, runID, "worker-a")
	checksDir := filepath.Join(root, runID, "checks")
	if err := os.MkdirAll(checksDir, 0755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(checksDir, "after-import.json"), []byte(`{"ok":true}`), 0644)
	_ = os.WriteFile(filepath.Join(checksDir, "after-run.json"), []byte(`{"ok":false}`), 0644)

	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.Consolidate(runID, minimalRunConfig(runID))
	if err != nil {
		t.Fatal(err)
	}
	if agg.Status.IntegrityOK {
		t.Fatalf("expected integrity_ok=false when a check failed, got %+v", agg.Status)
	}
	if len(agg.Status.IntegrityErrors) != 1 {
		t.Fatalf("expected one integrity error, got %#v", agg.Status.IntegrityErrors)
	}
	if !strings.Contains(agg.Status.IntegrityErrors[0], "after-run.json") || !strings.Contains(agg.Status.IntegrityErrors[0], "ok=false") {
		t.Fatalf("expected failed check reason, got %#v", agg.Status.IntegrityErrors[0])
	}

	if err := consolidate.WriteAggregate(root, runID, agg); err != nil {
		t.Fatal(err)
	}
	summary, err := os.ReadFile(filepath.Join(root, runID, "summary.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(summary), "integrity_error=check report after-run.json: ok=false") {
		t.Fatalf("summary missing integrity error details:\n%s", summary)
	}
}

func TestConsolidate_integritySkippedAfterRun(t *testing.T) {
	root := t.TempDir()
	runID := "run-skip-after-run"
	writeMinimalWorkerArtifacts(t, root, runID, "worker-a")
	checksDir := filepath.Join(root, runID, "checks")
	if err := os.MkdirAll(checksDir, 0755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(checksDir, "after-import.json"), []byte(`{"ok":true}`), 0644)

	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.ConsolidateWithOptions(runID, minimalRunConfig(runID), consolidate.Options{
		SkippedSteps:   []string{"check_after_run"},
		MaxClockSkewMs: 100,
	})
	if err != nil {
		t.Fatal(err)
	}
	if !agg.Status.IntegrityOK {
		t.Fatalf("expected integrity_ok=true when after-run check was skipped, got %+v", agg.Status)
	}
	if len(agg.Status.IntegrityErrors) != 0 {
		t.Fatalf("expected no integrity_errors, got %#v", agg.Status.IntegrityErrors)
	}
}

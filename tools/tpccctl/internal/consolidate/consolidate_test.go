package consolidate_test

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/tools/tpccctl/internal/canonical"
	"portable-tpcc/tools/tpccctl/internal/config"
	"portable-tpcc/tools/tpccctl/internal/consolidate"
	"portable-tpcc/tools/tpccctl/internal/state"
)

func writeRunConfig(t *testing.T, root, runID string, rc *config.RunConfig) string {
	t.Helper()
	orchDir := filepath.Join(root, runID, "orchestrator")
	if err := state.WriteJSON(orchDir, "run-config.json", rc); err != nil {
		t.Fatal(err)
	}
	sha, err := canonical.SHA256File(filepath.Join(orchDir, "run-config.json"))
	if err != nil {
		t.Fatal(err)
	}
	return sha
}

func writeWorkerArtifacts(t *testing.T, root, runID, workerName, sha string, rc *config.RunConfig, resultExtras map[string]interface{}) {
	t.Helper()
	var assign config.WorkerAssignmentJSON
	for _, w := range rc.WorkerAssignment {
		if w.Instance == workerName {
			assign = w
			break
		}
	}
	if assign.Instance == "" {
		assign = config.WorkerAssignmentJSON{
			Instance:        workerName,
			WarehouseRanges: [][]int{{1, 11}},
		}
	}
	workerDir := filepath.Join(root, runID, "raw", "worker", workerName)
	if err := os.MkdirAll(workerDir, 0755); err != nil {
		t.Fatal(err)
	}
	result := map[string]interface{}{
		"run_id":            runID,
		"instance":          workerName,
		"run_config_sha256": sha,
		"assignment": map[string]interface{}{
			"instance":         assign.Instance,
			"host":             assign.Host,
			"warehouse_ranges": assign.WarehouseRanges,
			"threads":          assign.Threads,
			"max_inflight":     assign.MaxInflight,
		},
		"exit_status": 0,
		"counters": map[string]interface{}{
			"new_order_ok": 10,
		},
	}
	for k, v := range resultExtras {
		result[k] = v
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

func writePassingChecks(t *testing.T, root, runID string) {
	t.Helper()
	checksDir := filepath.Join(root, runID, "checks")
	if err := os.MkdirAll(checksDir, 0755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(checksDir, "after-import.json"), []byte(`{"ok":true}`), 0644)
	_ = os.WriteFile(filepath.Join(checksDir, "after-run.json"), []byte(`{"ok":true}`), 0644)
}

func writeMinimalWorkerArtifacts(t *testing.T, root, runID, workerName string) string {
	t.Helper()
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, workerName, sha, rc, nil)
	return sha
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
			{Instance: "worker-a", Host: "host-a", WarehouseRanges: [][]int{{1, 11}}, Threads: 1, MaxInflight: 64},
		},
	}
}

func TestConsolidate_mergesHistograms(t *testing.T) {
	root := t.TempDir()
	runID := "run-hist"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
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
				"buckets":      []uint64{1, 1, 1, 1, 0, 0, 0, 0, 0},
			},
		},
	})
	writePassingChecks(t, root, runID)

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

func TestConsolidate_rejectsBadCounterType(t *testing.T) {
	root := t.TempDir()
	runID := "run-bad-counter"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"counters": map[string]interface{}{
			"new_order_ok": "ten",
		},
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil || !strings.Contains(err.Error(), "counter") {
		t.Fatalf("expected counter type error, got %v", err)
	}
}

func TestConsolidate_rejectsCorruptHistogram(t *testing.T) {
	root := t.TempDir()
	runID := "run-bad-hist"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"counters": map[string]interface{}{
			"new_order_ok": 10,
		},
		"histograms": map[string]interface{}{
			"new_order": map[string]interface{}{
				"layout":      "linear_exp",
				"unit":        "ms",
				"hdr_till":    4,
				"max_value":   64,
				"total_count": 4,
				"buckets":     []uint64{1, 1, 1, 1},
			},
		},
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil || !strings.Contains(err.Error(), "histogram") {
		t.Fatalf("expected histogram validation error, got %v", err)
	}
}

func TestConsolidate_clockSkewExceeded(t *testing.T) {
	root := t.TempDir()
	runID := "run-skew"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, nil)
	ready := map[string]interface{}{
		"clock_calibration": map[string]interface{}{
			"measured_at":    "2026-07-28T12:00:00Z",
			"offset_ms":      250.0,
			"uncertainty_ms": 2.0,
			"rtt_ms":         4.0,
		},
	}
	readyData, _ := json.MarshalIndent(ready, "", "  ")
	if err := os.WriteFile(filepath.Join(root, runID, "raw", "worker", "worker-a", "ready.json"), readyData, 0644); err != nil {
		t.Fatal(err)
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
	sha := writeRunConfig(t, root, runID, rc)
	for _, spec := range []struct {
		name   string
		offset float64
	}{
		{"worker-a", 5.0},
		{"worker-b", 120.0},
	} {
		writeWorkerArtifacts(t, root, runID, spec.name, sha, rc, nil)
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
		if err := os.WriteFile(filepath.Join(root, runID, "raw", "worker", spec.name, "ready.json"), readyData, 0644); err != nil {
			t.Fatal(err)
		}
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

func TestConsolidate_includesUserAbortedInThroughput(t *testing.T) {
	root := t.TempDir()
	runID := "run-tpmc-abort"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"counters": map[string]interface{}{
			"new_order_ok":           99,
			"new_order_user_aborted": 1,
		},
	})
	writePassingChecks(t, root, runID)

	// 60s measurement => 100 completed New-Orders / 1 min = 100 tpmC
	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.Consolidate(runID, rc)
	if err != nil {
		t.Fatal(err)
	}
	meas := agg.Metrics["measurement"].(map[string]interface{})
	if got := meas["new_order_count"].(int64); got != 100 {
		t.Fatalf("new_order_count=%v, want 100 (ok+user_aborted)", got)
	}
	if got := meas["new_order_ok"].(int64); got != 99 {
		t.Fatalf("new_order_ok=%v, want 99", got)
	}
	if got := meas["new_order_user_aborted"].(int64); got != 1 {
		t.Fatalf("new_order_user_aborted=%v, want 1", got)
	}
	if got := meas["throughput_new_order_per_min"].(float64); got != 100.0 {
		t.Fatalf("throughput=%v, want 100.0", got)
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

func TestConsolidate_rejectsUnexpectedWorker(t *testing.T) {
	root := t.TempDir()
	runID := "run-extra"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, nil)
	writeWorkerArtifacts(t, root, runID, "worker-stale", sha, rc, map[string]interface{}{
		"instance": "worker-stale",
		"assignment": map[string]interface{}{
			"instance":         "worker-stale",
			"warehouse_ranges": [][]int{{1, 11}},
		},
		"counters": map[string]interface{}{
			"new_order_ok": 999,
		},
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil {
		t.Fatal("expected error for unexpected worker artifact")
	}
	if !strings.Contains(err.Error(), "unexpected worker artifact") || !strings.Contains(err.Error(), "worker-stale") {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestConsolidate_rejectsMismatchedRunID(t *testing.T) {
	root := t.TempDir()
	runID := "run-id-mismatch"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"run_id": "other-run",
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil || !strings.Contains(err.Error(), "run_id") {
		t.Fatalf("expected run_id mismatch error, got %v", err)
	}
}

func TestConsolidate_rejectsMismatchedInstance(t *testing.T) {
	root := t.TempDir()
	runID := "run-instance-mismatch"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"instance": "worker-b",
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil || !strings.Contains(err.Error(), "instance") {
		t.Fatalf("expected instance mismatch error, got %v", err)
	}
}

func TestConsolidate_rejectsStaleRunConfigSHA(t *testing.T) {
	root := t.TempDir()
	runID := "run-stale-sha"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"run_config_sha256": "deadbeef",
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil || !strings.Contains(err.Error(), "run_config_sha256") {
		t.Fatalf("expected run_config_sha256 mismatch error, got %v", err)
	}
}

func TestConsolidate_rejectsMismatchedWarehouseAssignment(t *testing.T) {
	root := t.TempDir()
	runID := "run-wh-mismatch"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"assignment": map[string]interface{}{
			"instance":         "worker-a",
			"warehouse_ranges": [][]int{{1, 5}},
		},
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil || !strings.Contains(err.Error(), "warehouse_ranges") {
		t.Fatalf("expected warehouse_ranges mismatch error, got %v", err)
	}
}

package consolidate_test

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/mind/internal/canonical"
	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/consolidate"
	"portable-tpcc/mind/internal/state"
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
		"instance_nonce":    "nonce-" + workerName,
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
		"histograms": map[string]interface{}{
			"new_order": measurementHistogram(10),
		},
	}
	for k, v := range resultExtras {
		result[k] = v
	}
	resultData, _ := json.MarshalIndent(result, "", "  ")
	if err := os.WriteFile(filepath.Join(workerDir, "result.json"), resultData, 0644); err != nil {
		t.Fatal(err)
	}
	nonce, _ := result["instance_nonce"].(string)
	ready := map[string]interface{}{
		"instance_nonce": nonce,
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

func measurementHistogram(count uint64) map[string]interface{} {
	buckets := make([]uint64, 9)
	if count > 0 {
		buckets[0] = count
	}
	return map[string]interface{}{
		"layout":       "linear_exp",
		"unit":         "ms",
		"hdr_till":     4,
		"max_value":    64,
		"total_count":  count,
		"min_recorded": uint64(0),
		"max_recorded": uint64(0),
		"sum_values":   uint64(0),
		"buckets":      buckets,
	}
}

func patchWorkerResult(t *testing.T, root, runID, worker string, patch func(map[string]interface{})) {
	t.Helper()
	path := filepath.Join(root, runID, "raw", "worker", worker, "result.json")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var result map[string]interface{}
	if err := json.Unmarshal(data, &result); err != nil {
		t.Fatal(err)
	}
	patch(result)
	out, err := json.MarshalIndent(result, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, out, 0644); err != nil {
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
	_ = os.WriteFile(filepath.Join(checksDir, "after-test.json"), []byte(`{"ok":true}`), 0644)
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
			"new_order_ok": 4,
		},
		"histograms": map[string]interface{}{
			"new_order": map[string]interface{}{
				"layout":       "linear_exp",
				"unit":         "ms",
				"hdr_till":     4,
				"max_value":    64,
				"total_count":  4,
				"min_recorded": 0,
				"max_recorded": 3,
				"sum_values":   6,
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
	switch wh := meas["warehouses"].(type) {
	case int:
		if wh != 10 {
			t.Fatalf("expected consolidated warehouses=10, got %d", wh)
		}
	case int64:
		if wh != 10 {
			t.Fatalf("expected consolidated warehouses=10, got %d", wh)
		}
	default:
		t.Fatalf("expected consolidated warehouses=10 from worker assignment, got %#v", meas["warehouses"])
	}
	rt := meas["response_time_ms"].(map[string]interface{})
	stats, ok := rt["new_order"].(map[string]interface{})
	if !ok {
		t.Fatalf("missing response-time stats: %#v", rt)
	}
	if stats["min"].(uint64) != 0 || stats["max"].(uint64) != 3 {
		t.Fatalf("unexpected min/max: %#v", stats)
	}
	if avg := stats["avg"].(float64); avg != 1.5 {
		t.Fatalf("expected avg 1.5, got %v", avg)
	}
	if _, ok := stats["p50"]; !ok {
		t.Fatalf("missing percentiles: %#v", stats)
	}

	if err := consolidate.WriteAggregate(root, runID, agg); err != nil {
		t.Fatal(err)
	}
	summary, err := os.ReadFile(filepath.Join(root, runID, "summary.txt"))
	if err != nil {
		t.Fatal(err)
	}
	text := string(summary)
	if !strings.Contains(text, "=== TPC-C Results ===") {
		t.Fatalf("summary missing TPC-C Results header:\n%s", text)
	}
	if !strings.Contains(text, "New-Order Throughput:") || !strings.Contains(text, "tpmC") {
		t.Fatalf("summary missing tpmC line:\n%s", text)
	}
	if !strings.Contains(text, "NewOrder:") || !strings.Contains(text, "min=0ms max=3ms avg=1.5ms") {
		t.Fatalf("summary missing min/max/avg:\n%s", text)
	}
	if !strings.Contains(text, "Scale:") || !strings.Contains(text, "warehouses") {
		t.Fatalf("summary missing scale:\n%s", text)
	}
	if !strings.Contains(text, "p50=") || !strings.Contains(text, "p99=") {
		t.Fatalf("summary missing percentiles:\n%s", text)
	}
}

func TestFormatSummary_matchesTPCCResultsLayout(t *testing.T) {
	agg := &consolidate.Aggregate{
		RunID:       "run-demo",
		ResultClass: "engineering",
		Settings: config.SettingsForAggregate(&config.RunConfig{
			// Intentionally mismatched settings.scale so the summary must use
			// consolidated measurement.warehouses / worker_assignment coverage.
			Scale: config.ScaleBlock{Warehouses: 999},
			WorkerAssignment: []config.WorkerAssignmentJSON{
				{Instance: "worker-a", WarehouseRanges: [][]int{{1, 11}}},
			},
			Phases:  config.PhasesJSON{MeasurementMs: 60000},
			Runtime: config.RunRuntime{Pacing: "enabled"},
		}),
		Status: consolidate.Status{
			WorkersComplete:        true,
			AssignmentValid:        true,
			ClockSkewOK:            true,
			IntegrityOK:            true,
			TPCCSettingsConformant: true,
		},
		Metrics: map[string]interface{}{
			"measurement": map[string]interface{}{
				"new_order_count":              int64(100),
				"throughput_new_order_per_min": 100.0,
				"warehouses":                   10,
				"counters": map[string]int64{
					"new_order_ok":           99,
					"new_order_user_aborted": 1,
					"new_order_failed":       2,
					"payment_ok":             100,
					"payment_failed":         1,
				},
				"response_time_ms": map[string]interface{}{
					"new_order": map[string]interface{}{
						"min": uint64(1), "max": uint64(9), "avg": 2.5,
						"p50": uint64(2), "p90": uint64(4), "p95": uint64(5), "p99": uint64(8),
					},
					"payment": map[string]interface{}{
						"min": uint64(1), "max": uint64(5), "avg": 2.0,
						"p50": uint64(2), "p90": uint64(3), "p95": uint64(4), "p99": uint64(5),
					},
				},
			},
		},
	}
	text := consolidate.FormatSummary(agg)
	wantLines := []string{
		"run_id=run-demo result_class=engineering",
		"=== TPC-C Results ===",
		"  Scale: 10 warehouses",
		"  Measured Duration: 60.0s (configured: 60s)",
		"  New-Order Throughput: 100.00 tpmC",
		"  Efficiency: 77.8%",
		"  Total Failed: 3",
		"  NewOrder: OK=99 UserAborted=1 Failed=2 min=1ms max=9ms avg=2.5ms p50=2ms p90=4ms p99=8ms",
		"  Payment: OK=100 UserAborted=0 Failed=1 min=1ms max=5ms avg=2ms p50=2ms p90=3ms p99=5ms",
	}
	for _, want := range wantLines {
		if !strings.Contains(text, want) {
			t.Fatalf("summary missing %q:\n%s", want, text)
		}
	}
	if strings.Contains(text, "Scale: 999") {
		t.Fatalf("summary must not use settings.scale.warehouses:\n%s", text)
	}
}

func TestFormatSummary_latencyUnitsNormalizedToMs(t *testing.T) {
	agg := &consolidate.Aggregate{
		RunID:       "run-us",
		ResultClass: "engineering",
		Settings: config.SettingsForAggregate(&config.RunConfig{
			WorkerAssignment: []config.WorkerAssignmentJSON{
				{Instance: "worker-a", WarehouseRanges: [][]int{{1, 5}}},
			},
			Phases: config.PhasesJSON{MeasurementMs: 60000},
		}),
		Status: consolidate.Status{
			WorkersComplete: true, AssignmentValid: true, ClockSkewOK: true,
			IntegrityOK: true, TPCCSettingsConformant: true,
		},
		Metrics: map[string]interface{}{
			"measurement": map[string]interface{}{
				"throughput_new_order_per_min": 10.0,
				"warehouses":                   4,
				"counters": map[string]int64{
					"new_order_ok": 10,
				},
				"response_time_us": map[string]interface{}{
					"new_order": map[string]interface{}{
						"min": uint64(1500), "max": uint64(9000), "avg": 2500.0,
						"p50": uint64(2000), "p90": uint64(4000), "p99": uint64(8000),
					},
				},
			},
		},
	}
	text := consolidate.FormatSummary(agg)
	want := "  NewOrder: OK=10 UserAborted=0 Failed=0 min=1.5ms max=9ms avg=2.5ms p50=2ms p90=4ms p99=8ms"
	if !strings.Contains(text, want) {
		t.Fatalf("summary missing unified ms latencies %q:\n%s", want, text)
	}
	if !strings.Contains(text, "  Scale: 4 warehouses") {
		t.Fatalf("summary missing scale from consolidated warehouses:\n%s", text)
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

func TestConsolidate_rejectsMissingHistogramExtrema(t *testing.T) {
	root := t.TempDir()
	runID := "run-missing-extrema"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"counters": map[string]interface{}{
			"new_order_ok": 4,
		},
		"histograms": map[string]interface{}{
			"new_order": map[string]interface{}{
				"layout":      "linear_exp",
				"unit":        "ms",
				"hdr_till":    4,
				"max_value":   64,
				"total_count": 4,
				"buckets":     []uint64{1, 1, 1, 1, 0, 0, 0, 0, 0},
			},
		},
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil || !strings.Contains(err.Error(), "min_recorded") {
		t.Fatalf("expected missing min_recorded error, got %v", err)
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
		"instance_nonce": "nonce-worker-a",
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
			"instance_nonce": "nonce-" + spec.name,
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
		"histograms": map[string]interface{}{
			"new_order": measurementHistogram(100),
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

func TestConsolidate_tpccSettingsDeviationsInAggregate(t *testing.T) {
	root := t.TempDir()
	runID := "run-tpcc-settings"
	writeMinimalWorkerArtifacts(t, root, runID, "worker-a")
	writePassingChecks(t, root, runID)

	rc := minimalRunConfig(runID)
	rc.Workload = config.DefaultWorkload()
	rc.Workload.TerminalsPerWarehouse = 20
	rc.Runtime.Pacing = "disabled"
	rc.Runtime.ThinkTimeDistribution = "exponential"
	rc.Phases.MeasurementMs = 30 * 60 * 1000

	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.Consolidate(runID, rc)
	if err != nil {
		t.Fatal(err)
	}
	if agg.Status.TPCCSettingsConformant {
		t.Fatalf("expected tpcc_settings_conformant=false, status=%+v", agg.Status)
	}
	joined := strings.Join(agg.Status.TPCCSettingsDeviations, "\n")
	if !strings.Contains(joined, "terminals_per_warehouse=20") || !strings.Contains(joined, `pacing="disabled"`) {
		t.Fatalf("unexpected deviations: %#v", agg.Status.TPCCSettingsDeviations)
	}

	if err := consolidate.WriteAggregate(root, runID, agg); err != nil {
		t.Fatal(err)
	}
	summary, err := os.ReadFile(filepath.Join(root, runID, "summary.txt"))
	if err != nil {
		t.Fatal(err)
	}
	text := string(summary)
	if !strings.Contains(text, "tpcc_settings_conformant=false") {
		t.Fatalf("summary missing conformant flag:\n%s", text)
	}
	if !strings.Contains(text, "tpcc_settings_deviation=") {
		t.Fatalf("summary missing deviation lines:\n%s", text)
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
	if !strings.Contains(joined, "after-import.json") || !strings.Contains(joined, "after-test.json") {
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
	_ = os.WriteFile(filepath.Join(checksDir, "after-test.json"), []byte(`{"ok":false}`), 0644)

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
	if !strings.Contains(agg.Status.IntegrityErrors[0], "after-test.json") || !strings.Contains(agg.Status.IntegrityErrors[0], "ok=false") {
		t.Fatalf("expected failed check reason, got %#v", agg.Status.IntegrityErrors[0])
	}

	if err := consolidate.WriteAggregate(root, runID, agg); err != nil {
		t.Fatal(err)
	}
	summary, err := os.ReadFile(filepath.Join(root, runID, "summary.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(summary), "integrity_error=check report after-test.json: ok=false") {
		t.Fatalf("summary missing integrity error details:\n%s", summary)
	}
}

func TestConsolidate_integritySkippedAfterTest(t *testing.T) {
	root := t.TempDir()
	runID := "run-skip-after-test"
	writeMinimalWorkerArtifacts(t, root, runID, "worker-a")
	checksDir := filepath.Join(root, runID, "checks")
	if err := os.MkdirAll(checksDir, 0755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(checksDir, "after-import.json"), []byte(`{"ok":true}`), 0644)

	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.ConsolidateWithOptions(runID, minimalRunConfig(runID), consolidate.Options{
		SkippedSteps:   []string{"check_after_test"},
		MaxClockSkewMs: 100,
	})
	if err != nil {
		t.Fatal(err)
	}
	if !agg.Status.IntegrityOK {
		t.Fatalf("expected integrity_ok=true when after-test check was skipped, got %+v", agg.Status)
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

func TestConsolidateRejectsIncompleteWorkersByDefault(t *testing.T) {
	root := t.TempDir()
	runID := "run-incomplete"
	rc := minimalRunConfig(runID)
	writeRunConfig(t, root, runID, rc)
	workerDir := filepath.Join(root, runID, "raw", "worker", "worker-a")
	if err := os.MkdirAll(workerDir, 0755); err != nil {
		t.Fatal(err)
	}

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil || !strings.Contains(err.Error(), "incomplete worker artifacts") || !strings.Contains(err.Error(), "worker-a") {
		t.Fatalf("expected incomplete worker error, got %v", err)
	}
}

func TestConsolidateAllowIncompleteReturnsAggregate(t *testing.T) {
	root := t.TempDir()
	runID := "run-allow-incomplete"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"exit_status": 7,
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.ConsolidateWithOptions(runID, rc, consolidate.Options{
		MaxClockSkewMs:  100,
		AllowIncomplete: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	if agg.Status.WorkersComplete {
		t.Fatalf("expected workers_complete=false, got %+v", agg.Status)
	}
}

func TestConsolidate_rejectsIncompleteMeasurement(t *testing.T) {
	cases := []struct {
		name  string
		patch func(map[string]interface{})
		want  string
	}{
		{
			name:  "missing counters",
			patch: func(m map[string]interface{}) { delete(m, "counters") },
			want:  "missing counters",
		},
		{
			name:  "null counters",
			patch: func(m map[string]interface{}) { m["counters"] = nil },
			want:  "missing counters",
		},
		{
			name:  "missing histograms",
			patch: func(m map[string]interface{}) { delete(m, "histograms") },
			want:  "missing histograms",
		},
		{
			name:  "null histograms",
			patch: func(m map[string]interface{}) { m["histograms"] = nil },
			want:  "missing histograms",
		},
		{
			name:  "missing exit_status",
			patch: func(m map[string]interface{}) { delete(m, "exit_status") },
			want:  "missing exit_status",
		},
		{
			name:  "non-integer exit_status",
			patch: func(m map[string]interface{}) { m["exit_status"] = "0" },
			want:  "exit_status",
		},
		{
			name: "negative counter",
			patch: func(m map[string]interface{}) {
				m["counters"] = map[string]interface{}{"new_order_ok": -1}
				m["histograms"] = map[string]interface{}{}
			},
			want: "negative",
		},
		{
			name: "completed without histogram",
			patch: func(m map[string]interface{}) {
				m["counters"] = map[string]interface{}{"new_order_ok": 10}
				m["histograms"] = map[string]interface{}{}
			},
			want: "no response-time histogram",
		},
		{
			name: "histogram total_count mismatch",
			patch: func(m map[string]interface{}) {
				m["counters"] = map[string]interface{}{"new_order_ok": 10}
				m["histograms"] = map[string]interface{}{"new_order": measurementHistogram(4)}
			},
			want: "histogram.total_count",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			root := t.TempDir()
			runID := "run-malformed"
			rc := minimalRunConfig(runID)
			sha := writeRunConfig(t, root, runID, rc)
			writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, nil)
			patchWorkerResult(t, root, runID, "worker-a", tc.patch)

			cons := &consolidate.Consolidator{ResultRoot: root}
			_, err := cons.Consolidate(runID, rc)
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("expected error containing %q, got %v", tc.want, err)
			}
		})
	}
}

func TestConsolidate_allowsZeroCompletedWithoutHistogram(t *testing.T) {
	root := t.TempDir()
	runID := "run-zero-completed"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, map[string]interface{}{
		"counters":   map[string]interface{}{"new_order_failed": 3},
		"histograms": map[string]interface{}{},
	})
	writePassingChecks(t, root, runID)

	cons := &consolidate.Consolidator{ResultRoot: root}
	agg, err := cons.Consolidate(runID, rc)
	if err != nil {
		t.Fatal(err)
	}
	if !agg.Status.WorkersComplete {
		t.Fatalf("expected workers_complete=true, got %+v", agg.Status)
	}
	meas := agg.Metrics["measurement"].(map[string]interface{})
	if got := meas["new_order_count"].(int64); got != 0 {
		t.Fatalf("new_order_count=%v, want 0", got)
	}
}

func TestConsolidateAllowIncompleteStillRejectsMalformedMeasurement(t *testing.T) {
	root := t.TempDir()
	runID := "run-allow-malformed"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, nil)
	patchWorkerResult(t, root, runID, "worker-a", func(m map[string]interface{}) {
		delete(m, "counters")
	})

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.ConsolidateWithOptions(runID, rc, consolidate.Options{
		MaxClockSkewMs:  100,
		AllowIncomplete: true,
	})
	if err == nil || !strings.Contains(err.Error(), "missing counters") {
		t.Fatalf("expected malformed measurement error, got %v", err)
	}
}

func TestConsolidate_rejectsMismatchedInstanceNonce(t *testing.T) {
	root := t.TempDir()
	runID := "run-nonce-mismatch"
	rc := minimalRunConfig(runID)
	sha := writeRunConfig(t, root, runID, rc)
	writeWorkerArtifacts(t, root, runID, "worker-a", sha, rc, nil)
	readyPath := filepath.Join(root, runID, "raw", "worker", "worker-a", "ready.json")
	ready := map[string]interface{}{
		"instance_nonce": "stale-nonce",
		"clock_calibration": map[string]interface{}{
			"measured_at":    "2026-07-28T12:00:00Z",
			"offset_ms":      5.0,
			"uncertainty_ms": 2.0,
			"rtt_ms":         4.0,
		},
	}
	readyData, _ := json.MarshalIndent(ready, "", "  ")
	if err := os.WriteFile(readyPath, readyData, 0644); err != nil {
		t.Fatal(err)
	}

	cons := &consolidate.Consolidator{ResultRoot: root}
	_, err := cons.Consolidate(runID, rc)
	if err == nil || !strings.Contains(err.Error(), "instance_nonce") {
		t.Fatalf("expected instance_nonce mismatch error, got %v", err)
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

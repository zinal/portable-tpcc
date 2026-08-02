package consolidate

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"portable-tpcc/tools/tpccctl/internal/config"
	"portable-tpcc/tools/tpccctl/internal/histogram"
)

// Status flags for the consolidated result (specification §8.2).
type Status struct {
	WorkersComplete bool `json:"workers_complete"`
	AssignmentValid bool `json:"assignment_valid"`
	ClockSkewOK     bool `json:"clock_skew_ok"`
	IntegrityOK     bool `json:"integrity_ok"`
}

// Aggregate is the canonical consolidated result.
// It embeds concrete run settings rather than config hashes.
type Aggregate struct {
	SchemaVersion int                    `json:"schema_version"`
	RunID         string                 `json:"run_id"`
	ResultClass   string                 `json:"result_class"`
	Settings      map[string]interface{} `json:"settings"`
	Status        Status                 `json:"status"`
	Metrics       map[string]interface{} `json:"metrics"`
	Workers       []string               `json:"workers"`
	SkippedSteps  []string               `json:"skipped_steps,omitempty"`
	Checks        map[string]interface{} `json:"checks,omitempty"`
}

// Options tune consolidate status evaluation.
type Options struct {
	SkippedSteps []string
	MaxClockSkewMs int64
}

// Consolidator merges worker artifacts deterministically.
type Consolidator struct {
	ResultRoot string
}

// Consolidate builds aggregate.json from collected artifacts.
func (c *Consolidator) Consolidate(runID string, rc *config.RunConfig) (*Aggregate, error) {
	return c.ConsolidateWithOptions(runID, rc, Options{MaxClockSkewMs: rc.Phases.MaxClockSkewMs})
}

// ConsolidateWithOptions builds aggregate.json with explicit status inputs.
func (c *Consolidator) ConsolidateWithOptions(runID string, rc *config.RunConfig, opts Options) (*Aggregate, error) {
	rawWorkers := filepath.Join(c.ResultRoot, runID, "raw", "worker")
	entries, err := os.ReadDir(rawWorkers)
	if err != nil {
		return nil, err
	}
	expected := config.ExpectedWorkerNames(rc)
	assignmentErr := config.ValidateRunConfigAssignment(rc)

	present := map[string]bool{}
	counters := map[string]int64{}
	mergedHist := map[string]histogram.Raw{}
	workersComplete := true
	maxSkew := opts.MaxClockSkewMs
	if maxSkew <= 0 {
		maxSkew = rc.Phases.MaxClockSkewMs
	}
	workerCalibrations := map[string]workerClockCalibration{}

	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		present[e.Name()] = true
		resultPath := filepath.Join(rawWorkers, e.Name(), "result.json")
		data, err := os.ReadFile(resultPath)
		if err != nil {
			workersComplete = false
			continue
		}
		var partial map[string]interface{}
		if err := json.Unmarshal(data, &partial); err != nil {
			workersComplete = false
			continue
		}
		if exit, ok := partial["exit_status"].(float64); ok && int(exit) != 0 {
			workersComplete = false
		}
		if ctr, ok := partial["counters"].(map[string]interface{}); ok {
			for k, v := range ctr {
				if n, ok := v.(float64); ok {
					counters[k] += int64(n)
				}
			}
		}
		if hists, ok := partial["histograms"].(map[string]interface{}); ok {
			for tx, raw := range hists {
				bytes, err := json.Marshal(raw)
				if err != nil {
					continue
				}
				var h histogram.Raw
				if err := json.Unmarshal(bytes, &h); err != nil {
					continue
				}
				cur := mergedHist[tx]
				if err := histogram.Merge(&cur, h); err != nil {
					return nil, fmt.Errorf("merge histogram %s from %s: %w", tx, e.Name(), err)
				}
				mergedHist[tx] = cur
			}
		}
		if readyPath := filepath.Join(rawWorkers, e.Name(), "ready.json"); true {
			if cal, ok := readWorkerClockCalibration(readyPath); ok {
				workerCalibrations[e.Name()] = cal
			}
		}
	}
	for _, name := range expected {
		if !present[name] {
			workersComplete = false
		}
	}
	clockSkewOK := evaluateClockSkew(workerCalibrations, maxSkew, expected)

	responseTimes := map[string]interface{}{}
	unit := "ms"
	for tx, h := range mergedHist {
		if h.Unit != "" {
			unit = h.Unit
		}
		pct, err := histogram.Percentiles(h)
		if err != nil {
			return nil, err
		}
		responseTimes[tx] = pct
	}

	newOrder := counters["new_order_ok"]
	measurementMin := float64(rc.Phases.MeasurementMs) / 60000.0
	throughput := 0.0
	if measurementMin > 0 {
		throughput = float64(newOrder) / measurementMin
	}

	integrityOK := evaluateIntegrity(c.ResultRoot, runID)

	agg := &Aggregate{
		SchemaVersion: 1,
		RunID:         runID,
		ResultClass:   "engineering",
		Settings:      config.SettingsForAggregate(rc),
		Status: Status{
			WorkersComplete: workersComplete,
			AssignmentValid: assignmentErr == nil,
			ClockSkewOK:     clockSkewOK,
			IntegrityOK:     integrityOK,
		},
		Metrics: map[string]interface{}{
			"measurement": map[string]interface{}{
				"new_order_count":              newOrder,
				"throughput_new_order_per_min": throughput,
				"counters":                     counters,
				"response_time_" + unit:        responseTimes,
			},
		},
		Workers:      expected,
		SkippedSteps: opts.SkippedSteps,
	}
	if checks := loadChecks(c.ResultRoot, runID); checks != nil {
		agg.Checks = checks
	}
	if assignmentErr != nil && len(entries) == 0 {
		return nil, assignmentErr
	}
	if len(expected) > 0 && len(present) == 0 {
		return nil, fmt.Errorf("no worker artifacts under %s", rawWorkers)
	}
	for _, name := range expected {
		if !present[name] {
			return nil, fmt.Errorf("missing worker artifact for %s", name)
		}
	}
	return agg, nil
}

func absFloat(v float64) float64 {
	if v < 0 {
		return -v
	}
	return v
}

type workerClockCalibration struct {
	offset      float64
	uncertainty float64
}

func readWorkerClockCalibration(readyPath string) (workerClockCalibration, bool) {
	readyData, err := os.ReadFile(readyPath)
	if err != nil {
		return workerClockCalibration{}, false
	}
	var ready map[string]interface{}
	if json.Unmarshal(readyData, &ready) != nil {
		return workerClockCalibration{}, false
	}
	cal, ok := ready["clock_calibration"].(map[string]interface{})
	if !ok {
		return workerClockCalibration{}, false
	}
	off, hasOff := cal["offset_ms"].(float64)
	unc, hasUnc := cal["uncertainty_ms"].(float64)
	_, hasMeasured := cal["measured_at"].(string)
	if !hasOff || !hasUnc || !hasMeasured {
		return workerClockCalibration{}, false
	}
	return workerClockCalibration{offset: off, uncertainty: unc}, true
}

// evaluateClockSkew checks per-worker offset vs the reference time source and the
// spread across workers. The spread check is a hedge for distributed databases where
// each worker may calibrate against a different node.
func evaluateClockSkew(workerCalibrations map[string]workerClockCalibration, maxSkew int64, expected []string) bool {
	if maxSkew <= 0 {
		return true
	}
	if len(expected) > 0 && len(workerCalibrations) != len(expected) {
		return false
	}
	if len(workerCalibrations) == 0 {
		return false
	}
	minOffset := 0.0
	maxOffset := 0.0
	first := true
	for _, cal := range workerCalibrations {
		if absFloat(cal.offset) > float64(maxSkew) || absFloat(cal.uncertainty) > float64(maxSkew) {
			return false
		}
		if first {
			minOffset = cal.offset
			maxOffset = cal.offset
			first = false
			continue
		}
		if cal.offset < minOffset {
			minOffset = cal.offset
		}
		if cal.offset > maxOffset {
			maxOffset = cal.offset
		}
	}
	if len(workerCalibrations) >= 2 && maxOffset-minOffset > float64(maxSkew) {
		return false
	}
	return true
}

func evaluateIntegrity(resultRoot, runID string) bool {
	checksDir := filepath.Join(resultRoot, runID, "checks")
	entries, err := os.ReadDir(checksDir)
	if err != nil {
		// No checks collected yet — treat as unknown/ok for consolidate of measurement-only runs.
		return true
	}
	ok := true
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		data, err := os.ReadFile(filepath.Join(checksDir, e.Name()))
		if err != nil {
			ok = false
			continue
		}
		var report map[string]interface{}
		if json.Unmarshal(data, &report) != nil {
			ok = false
			continue
		}
		if v, exists := report["ok"].(bool); exists && !v {
			ok = false
		}
	}
	return ok
}

func loadChecks(resultRoot, runID string) map[string]interface{} {
	checksDir := filepath.Join(resultRoot, runID, "checks")
	entries, err := os.ReadDir(checksDir)
	if err != nil {
		return nil
	}
	out := map[string]interface{}{}
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		data, err := os.ReadFile(filepath.Join(checksDir, e.Name()))
		if err != nil {
			continue
		}
		var report interface{}
		if json.Unmarshal(data, &report) != nil {
			continue
		}
		out[e.Name()] = report
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

// WriteAggregate writes aggregate.json and summary.txt.
func WriteAggregate(resultRoot, runID string, agg *Aggregate) error {
	dir := filepath.Join(resultRoot, runID)
	data, err := json.MarshalIndent(agg, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	tmp := filepath.Join(dir, "aggregate.json.tmp")
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	if err := os.Rename(tmp, filepath.Join(dir, "aggregate.json")); err != nil {
		return err
	}
	summary := fmt.Sprintf(
		"run_id=%s result_class=%s workers_complete=%v assignment_valid=%v clock_skew_ok=%v integrity_ok=%v\n",
		agg.RunID, agg.ResultClass, agg.Status.WorkersComplete, agg.Status.AssignmentValid,
		agg.Status.ClockSkewOK, agg.Status.IntegrityOK,
	)
	return os.WriteFile(filepath.Join(dir, "summary.txt"), []byte(summary), 0644)
}

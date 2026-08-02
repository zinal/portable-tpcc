package consolidate

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"

	"portable-tpcc/tpccctl/internal/canonical"
	"portable-tpcc/tpccctl/internal/config"
	"portable-tpcc/tpccctl/internal/histogram"
)

// Status flags for the consolidated result (specification §8.2).
type Status struct {
	WorkersComplete        bool     `json:"workers_complete"`
	AssignmentValid        bool     `json:"assignment_valid"`
	ClockSkewOK            bool     `json:"clock_skew_ok"`
	IntegrityOK            bool     `json:"integrity_ok"`
	IntegrityErrors        []string `json:"integrity_errors,omitempty"`
	TPCCSettingsConformant bool     `json:"tpcc_settings_conformant"`
	TPCCSettingsDeviations []string `json:"tpcc_settings_deviations,omitempty"`
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
	SkippedSteps            []string
	MaxClockSkewMs          int64
	ExpectedRunConfigSHA256 string
	AllowIncomplete         bool
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
	expectedSet := make(map[string]bool, len(expected))
	for _, name := range expected {
		expectedSet[name] = true
	}
	expectedAssign := make(map[string]config.WorkerAssignmentJSON, len(rc.WorkerAssignment))
	for _, w := range rc.WorkerAssignment {
		expectedAssign[w.Instance] = w
	}
	expectedSHA, err := resolveExpectedRunConfigSHA256(c.ResultRoot, runID, opts)
	if err != nil {
		return nil, err
	}

	present := map[string]bool{}
	counters := map[string]int64{}
	mergedHist := map[string]histogram.Raw{}
	workersComplete := true
	maxSkew := opts.MaxClockSkewMs
	if maxSkew <= 0 {
		maxSkew = rc.Phases.MaxClockSkewMs
	}
	workerCalibrations := map[string]workerClockCalibration{}
	var incompleteWorkers []string

	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		name := e.Name()
		if !expectedSet[name] {
			return nil, fmt.Errorf("unexpected worker artifact %q (not in expected set %v)", name, expected)
		}
		present[name] = true
		resultPath := filepath.Join(rawWorkers, name, "result.json")
		data, err := os.ReadFile(resultPath)
		if err != nil {
			workersComplete = false
			incompleteWorkers = append(incompleteWorkers, fmt.Sprintf("%s: missing result.json", name))
			continue
		}
		var partial map[string]interface{}
		if err := json.Unmarshal(data, &partial); err != nil {
			return nil, fmt.Errorf("worker %s: invalid result.json: %w", name, err)
		}
		if err := validateWorkerResultIdentity(partial, runID, name, expectedSHA, expectedAssign[name]); err != nil {
			return nil, fmt.Errorf("worker %s: %w", name, err)
		}
		if err := validateWorkerNonceConsistency(rawWorkers, name, partial); err != nil {
			return nil, fmt.Errorf("worker %s: %w", name, err)
		}
		if exit, ok := partial["exit_status"].(float64); ok && int(exit) != 0 {
			workersComplete = false
			incompleteWorkers = append(incompleteWorkers, fmt.Sprintf("%s: exit_status=%d", name, int(exit)))
		}
		if err := mergeWorkerCounters(counters, partial["counters"], name); err != nil {
			return nil, err
		}
		if err := mergeWorkerHistograms(mergedHist, partial["histograms"], name); err != nil {
			return nil, err
		}
		if readyPath := filepath.Join(rawWorkers, name, "ready.json"); true {
			if cal, ok := readWorkerClockCalibration(readyPath); ok {
				workerCalibrations[name] = cal
			}
		}
	}
	for _, name := range expected {
		if !present[name] {
			workersComplete = false
			incompleteWorkers = append(incompleteWorkers, fmt.Sprintf("%s: missing worker artifact", name))
		}
	}
	clockSkewOK := evaluateClockSkew(workerCalibrations, maxSkew, expected)

	responseTimes := map[string]interface{}{}
	unit := "ms"
	for tx, h := range mergedHist {
		if h.Unit != "" {
			unit = h.Unit
		}
		stats, err := histogram.ReportStats(h)
		if err != nil {
			return nil, err
		}
		responseTimes[tx] = stats
	}

	// TPC-C §5.1.2 / §5.4.2: intentional unused-item New-Order rollbacks are
	// completed transactions and must be included in MQTh (tpmC).
	newOrderOk := counters["new_order_ok"]
	newOrderUserAborted := counters["new_order_user_aborted"]
	newOrder := newOrderOk + newOrderUserAborted
	measurementMin := float64(rc.Phases.MeasurementMs) / 60000.0
	throughput := 0.0
	if measurementMin > 0 {
		throughput = float64(newOrder) / measurementMin
	}

	integrity := evaluateIntegrity(c.ResultRoot, runID, opts)
	tpccDevs := config.TPCSettingsDeviations(rc)

	agg := &Aggregate{
		SchemaVersion: 1,
		RunID:         runID,
		ResultClass:   "engineering",
		Settings:      config.SettingsForAggregate(rc),
		Status: Status{
			WorkersComplete:        workersComplete,
			AssignmentValid:        assignmentErr == nil,
			ClockSkewOK:            clockSkewOK,
			IntegrityOK:            integrity.ok,
			IntegrityErrors:        integrity.errors,
			TPCCSettingsConformant: len(tpccDevs) == 0,
			TPCCSettingsDeviations: tpccDevs,
		},
		Metrics: map[string]interface{}{
			"measurement": map[string]interface{}{
				"new_order_count":              newOrder,
				"new_order_ok":                 newOrderOk,
				"new_order_user_aborted":       newOrderUserAborted,
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
	if !workersComplete && !opts.AllowIncomplete {
		return nil, fmt.Errorf("incomplete worker artifacts: %s", strings.Join(incompleteWorkers, "; "))
	}
	return agg, nil
}

func mergeWorkerCounters(counters map[string]int64, raw interface{}, worker string) error {
	if raw == nil {
		return nil
	}
	ctr, ok := raw.(map[string]interface{})
	if !ok {
		return fmt.Errorf("worker %s: counters must be an object, got %T", worker, raw)
	}
	for k, v := range ctr {
		n, err := jsonNumberAsInt64(v)
		if err != nil {
			return fmt.Errorf("worker %s: counter %q: %w", worker, k, err)
		}
		counters[k] += n
	}
	return nil
}

func mergeWorkerHistograms(merged map[string]histogram.Raw, raw interface{}, worker string) error {
	if raw == nil {
		return nil
	}
	hists, ok := raw.(map[string]interface{})
	if !ok {
		return fmt.Errorf("worker %s: histograms must be an object, got %T", worker, raw)
	}
	for tx, payload := range hists {
		bytes, err := json.Marshal(payload)
		if err != nil {
			return fmt.Errorf("worker %s: histogram %s: encode: %w", worker, tx, err)
		}
		var h histogram.Raw
		if err := json.Unmarshal(bytes, &h); err != nil {
			return fmt.Errorf("worker %s: histogram %s: decode: %w", worker, tx, err)
		}
		if err := histogram.Validate(h); err != nil {
			return fmt.Errorf("worker %s: histogram %s: %w", worker, tx, err)
		}
		cur := merged[tx]
		if err := histogram.Merge(&cur, h); err != nil {
			return fmt.Errorf("merge histogram %s from %s: %w", tx, worker, err)
		}
		merged[tx] = cur
	}
	return nil
}

func jsonNumberAsInt64(v interface{}) (int64, error) {
	switch n := v.(type) {
	case float64:
		if n != float64(int64(n)) || n > float64(math.MaxInt64) || n < float64(math.MinInt64) {
			return 0, fmt.Errorf("expected integer, got %v", n)
		}
		return int64(n), nil
	case json.Number:
		i, err := n.Int64()
		if err != nil {
			return 0, fmt.Errorf("expected integer, got %v", n)
		}
		return i, nil
	case int:
		return int64(n), nil
	case int64:
		return n, nil
	default:
		return 0, fmt.Errorf("expected integer, got %T", v)
	}
}

func resolveExpectedRunConfigSHA256(resultRoot, runID string, opts Options) (string, error) {
	if opts.ExpectedRunConfigSHA256 != "" {
		return opts.ExpectedRunConfigSHA256, nil
	}
	path := filepath.Join(resultRoot, runID, "orchestrator", "run-config.json")
	sha, err := canonical.SHA256File(path)
	if err != nil {
		return "", fmt.Errorf("resolve run_config_sha256 from %s: %w", path, err)
	}
	return sha, nil
}

func validateWorkerNonceConsistency(rawWorkers, name string, result map[string]interface{}) error {
	resultNonce, _ := result["instance_nonce"].(string)
	if resultNonce == "" {
		return fmt.Errorf("result.json missing instance_nonce")
	}
	for _, artifact := range []string{"ready.json", "process.json", "artifact-manifest.json"} {
		path := filepath.Join(rawWorkers, name, artifact)
		data, err := os.ReadFile(path)
		if err != nil {
			if os.IsNotExist(err) {
				// ready/process/manifest may be absent for incomplete workers handled elsewhere.
				continue
			}
			return err
		}
		var meta map[string]interface{}
		if err := json.Unmarshal(data, &meta); err != nil {
			return fmt.Errorf("%s: invalid JSON: %w", artifact, err)
		}
		nonce, _ := meta["instance_nonce"].(string)
		if nonce == "" {
			return fmt.Errorf("%s missing instance_nonce", artifact)
		}
		if nonce != resultNonce {
			return fmt.Errorf("%s instance_nonce %q does not match result.json %q", artifact, nonce, resultNonce)
		}
	}
	return nil
}

func validateWorkerResultIdentity(
	partial map[string]interface{},
	runID, instance, expectedSHA string,
	expected config.WorkerAssignmentJSON,
) error {
	gotRunID, _ := partial["run_id"].(string)
	if gotRunID == "" {
		return fmt.Errorf("result.json missing run_id")
	}
	if gotRunID != runID {
		return fmt.Errorf("run_id %q does not match consolidate run_id %q", gotRunID, runID)
	}
	gotInstance, _ := partial["instance"].(string)
	if gotInstance == "" {
		return fmt.Errorf("result.json missing instance")
	}
	if gotInstance != instance {
		return fmt.Errorf("instance %q does not match directory %q", gotInstance, instance)
	}
	gotSHA, _ := partial["run_config_sha256"].(string)
	if gotSHA == "" {
		return fmt.Errorf("result.json missing run_config_sha256")
	}
	if gotSHA != expectedSHA {
		return fmt.Errorf("run_config_sha256 %q does not match expected %q", gotSHA, expectedSHA)
	}
	assign, ok := partial["assignment"].(map[string]interface{})
	if !ok {
		return fmt.Errorf("result.json missing assignment")
	}
	if assignInstance, _ := assign["instance"].(string); assignInstance != "" && assignInstance != instance {
		return fmt.Errorf("assignment.instance %q does not match %q", assignInstance, instance)
	}
	gotRanges, err := parseWarehouseRanges(assign["warehouse_ranges"])
	if err != nil {
		return fmt.Errorf("assignment.warehouse_ranges: %w", err)
	}
	if !reflect.DeepEqual(gotRanges, expected.WarehouseRanges) {
		return fmt.Errorf("warehouse_ranges %v do not match expected %v", gotRanges, expected.WarehouseRanges)
	}
	return nil
}

func parseWarehouseRanges(v interface{}) ([][]int, error) {
	arr, ok := v.([]interface{})
	if !ok {
		return nil, fmt.Errorf("expected array, got %T", v)
	}
	out := make([][]int, 0, len(arr))
	for i, item := range arr {
		pair, ok := item.([]interface{})
		if !ok || len(pair) != 2 {
			return nil, fmt.Errorf("entry %d must be [start, end)", i)
		}
		start, okStart := jsonNumberAsInt(pair[0])
		end, okEnd := jsonNumberAsInt(pair[1])
		if !okStart || !okEnd {
			return nil, fmt.Errorf("entry %d bounds must be integers", i)
		}
		out = append(out, []int{start, end})
	}
	return out, nil
}

func jsonNumberAsInt(v interface{}) (int, bool) {
	switch n := v.(type) {
	case float64:
		if n != float64(int(n)) {
			return 0, false
		}
		return int(n), true
	case json.Number:
		i, err := n.Int64()
		if err != nil {
			return 0, false
		}
		return int(i), true
	case int:
		return n, true
	case int64:
		return int(n), true
	default:
		return 0, false
	}
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

var checkPhaseFiles = []struct {
	skipStep string
	fileName string
}{
	{"check_after_import", "after-import.json"},
	{"check_after_run", "after-run.json"},
}

func skippedStepSet(steps []string) map[string]bool {
	out := make(map[string]bool, len(steps))
	for _, s := range steps {
		out[s] = true
	}
	return out
}

func requiredCheckFiles(skipped []string) []string {
	skip := skippedStepSet(skipped)
	var required []string
	for _, p := range checkPhaseFiles {
		if !skip[p.skipStep] {
			required = append(required, p.fileName)
		}
	}
	return required
}

func checkReportOK(data []byte) (bool, string) {
	var report map[string]interface{}
	if json.Unmarshal(data, &report) != nil {
		return false, "invalid JSON"
	}
	ok, exists := report["ok"].(bool)
	if !exists {
		return false, "missing ok field"
	}
	if !ok {
		if failed, exists := report["failed"].(float64); exists && failed > 0 {
			return false, fmt.Sprintf("ok=false (failed=%d)", int(failed))
		}
		return false, "ok=false"
	}
	return true, ""
}

type integrityEvaluation struct {
	ok     bool
	errors []string
}

func evaluateIntegrity(resultRoot, runID string, opts Options) integrityEvaluation {
	required := requiredCheckFiles(opts.SkippedSteps)
	if len(required) == 0 {
		return integrityEvaluation{ok: true}
	}

	checksDir := filepath.Join(resultRoot, runID, "checks")
	present := map[string]bool{}
	var errors []string

	entries, err := os.ReadDir(checksDir)
	if err != nil {
		if os.IsNotExist(err) {
			for _, name := range required {
				errors = append(errors, fmt.Sprintf("required check report missing: %s", name))
			}
			return integrityEvaluation{ok: false, errors: errors}
		}
		return integrityEvaluation{
			ok:     false,
			errors: []string{fmt.Sprintf("checks directory unreadable: %v", err)},
		}
	}

	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		present[e.Name()] = true
		path := filepath.Join(checksDir, e.Name())
		data, err := os.ReadFile(path)
		if err != nil {
			errors = append(errors, fmt.Sprintf("check report %s unreadable: %v", e.Name(), err))
			continue
		}
		if ok, reason := checkReportOK(data); !ok {
			errors = append(errors, fmt.Sprintf("check report %s: %s", e.Name(), reason))
		}
	}
	for _, name := range required {
		if !present[name] {
			errors = append(errors, fmt.Sprintf("required check report missing: %s", name))
		}
	}
	return integrityEvaluation{ok: len(errors) == 0, errors: errors}
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
	summary := formatSummary(agg)
	return os.WriteFile(filepath.Join(dir, "summary.txt"), []byte(summary), 0644)
}

func formatSummary(agg *Aggregate) string {
	var b strings.Builder
	fmt.Fprintf(&b,
		"run_id=%s result_class=%s workers_complete=%v assignment_valid=%v clock_skew_ok=%v integrity_ok=%v tpcc_settings_conformant=%v\n",
		agg.RunID, agg.ResultClass, agg.Status.WorkersComplete, agg.Status.AssignmentValid,
		agg.Status.ClockSkewOK, agg.Status.IntegrityOK, agg.Status.TPCCSettingsConformant,
	)
	if !agg.Status.IntegrityOK && len(agg.Status.IntegrityErrors) > 0 {
		for _, errMsg := range agg.Status.IntegrityErrors {
			fmt.Fprintf(&b, "integrity_error=%s\n", errMsg)
		}
	}
	if !agg.Status.TPCCSettingsConformant {
		for _, d := range agg.Status.TPCCSettingsDeviations {
			fmt.Fprintf(&b, "tpcc_settings_deviation=%s\n", d)
		}
	}
	if meas, ok := agg.Metrics["measurement"].(map[string]interface{}); ok {
		if v, ok := meas["throughput_new_order_per_min"]; ok {
			fmt.Fprintf(&b, "throughput_new_order_per_min=%v\n", v)
		}
		appendResponseTimeSummary(&b, meas)
	}
	return b.String()
}

func appendResponseTimeSummary(b *strings.Builder, meas map[string]interface{}) {
	var rtKey string
	var rt map[string]interface{}
	for k, v := range meas {
		if !strings.HasPrefix(k, "response_time_") {
			continue
		}
		m, ok := v.(map[string]interface{})
		if !ok {
			continue
		}
		rtKey = k
		rt = m
		break
	}
	if rt == nil {
		return
	}
	txs := make([]string, 0, len(rt))
	for tx := range rt {
		txs = append(txs, tx)
	}
	sort.Strings(txs)
	for _, tx := range txs {
		stats, ok := rt[tx].(map[string]interface{})
		if !ok {
			continue
		}
		fmt.Fprintf(b, "%s.%s min=%v max=%v avg=%v p50=%v p90=%v p95=%v p99=%v\n",
			rtKey, tx,
			stats["min"], stats["max"], stats["avg"],
			stats["p50"], stats["p90"], stats["p95"], stats["p99"])
	}
}

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

	"portable-tpcc/mind/internal/canonical"
	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/histogram"
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
	totalWarehouses := 0

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
		exit, err := requireExitStatus(partial["exit_status"], name)
		if err != nil {
			return nil, err
		}
		if exit != 0 {
			workersComplete = false
			incompleteWorkers = append(incompleteWorkers, fmt.Sprintf("%s: exit_status=%d", name, exit))
		}
		workerCounters, err := parseWorkerCounters(partial["counters"], name)
		if err != nil {
			return nil, err
		}
		workerHists, err := parseWorkerHistograms(partial["histograms"], name)
		if err != nil {
			return nil, err
		}
		if err := validateCounterHistogramInvariants(workerCounters, workerHists, name); err != nil {
			return nil, err
		}
		for k, v := range workerCounters {
			counters[k] += v
		}
		for tx, h := range workerHists {
			cur := mergedHist[tx]
			if err := histogram.Merge(&cur, h); err != nil {
				return nil, fmt.Errorf("merge histogram %s from %s: %w", tx, name, err)
			}
			mergedHist[tx] = cur
		}
		totalWarehouses += workerWarehouseCount(partial, expectedAssign[name])
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
				"warehouses":                   totalWarehouses,
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

func requireExitStatus(raw interface{}, worker string) (int64, error) {
	if raw == nil {
		return 0, fmt.Errorf("worker %s: missing exit_status", worker)
	}
	n, err := jsonNumberAsInt64(raw)
	if err != nil {
		return 0, fmt.Errorf("worker %s: exit_status: %w", worker, err)
	}
	return n, nil
}

func parseWorkerCounters(raw interface{}, worker string) (map[string]int64, error) {
	if raw == nil {
		return nil, fmt.Errorf("worker %s: missing counters", worker)
	}
	ctr, ok := raw.(map[string]interface{})
	if !ok {
		return nil, fmt.Errorf("worker %s: counters must be an object, got %T", worker, raw)
	}
	out := make(map[string]int64, len(ctr))
	for k, v := range ctr {
		n, err := jsonNumberAsInt64(v)
		if err != nil {
			return nil, fmt.Errorf("worker %s: counter %q: %w", worker, k, err)
		}
		if n < 0 {
			return nil, fmt.Errorf("worker %s: counter %q is negative (%d)", worker, k, n)
		}
		out[k] = n
	}
	return out, nil
}

func parseWorkerHistograms(raw interface{}, worker string) (map[string]histogram.Raw, error) {
	if raw == nil {
		return nil, fmt.Errorf("worker %s: missing histograms", worker)
	}
	hists, ok := raw.(map[string]interface{})
	if !ok {
		return nil, fmt.Errorf("worker %s: histograms must be an object, got %T", worker, raw)
	}
	out := make(map[string]histogram.Raw, len(hists))
	for tx, payload := range hists {
		bytes, err := json.Marshal(payload)
		if err != nil {
			return nil, fmt.Errorf("worker %s: histogram %s: encode: %w", worker, tx, err)
		}
		var h histogram.Raw
		if err := json.Unmarshal(bytes, &h); err != nil {
			return nil, fmt.Errorf("worker %s: histogram %s: decode: %w", worker, tx, err)
		}
		if err := histogram.Validate(h); err != nil {
			return nil, fmt.Errorf("worker %s: histogram %s: %w", worker, tx, err)
		}
		out[tx] = h
	}
	return out, nil
}

var counterSuffixes = []string{"_user_aborted", "_retried", "_failed", "_ok"}

func transactionTypeFromCounter(key string) (string, bool) {
	for _, suffix := range counterSuffixes {
		if strings.HasSuffix(key, suffix) && len(key) > len(suffix) {
			return strings.TrimSuffix(key, suffix), true
		}
	}
	return "", false
}

func completedCount(counters map[string]int64, tx string) (int64, error) {
	ok := counters[tx+"_ok"]
	aborted := counters[tx+"_user_aborted"]
	if aborted > 0 && ok > math.MaxInt64-aborted {
		return 0, fmt.Errorf("%s completed count overflows", tx)
	}
	return ok + aborted, nil
}

func validateCounterHistogramInvariants(counters map[string]int64, hists map[string]histogram.Raw, worker string) error {
	seen := make(map[string]bool, len(counters)+len(hists))
	for k := range counters {
		if tx, ok := transactionTypeFromCounter(k); ok {
			seen[tx] = true
		}
	}
	for tx := range hists {
		seen[tx] = true
	}
	txs := make([]string, 0, len(seen))
	for tx := range seen {
		txs = append(txs, tx)
	}
	sort.Strings(txs)
	for _, tx := range txs {
		completed, err := completedCount(counters, tx)
		if err != nil {
			return fmt.Errorf("worker %s: %w", worker, err)
		}
		h, hasHist := hists[tx]
		if completed > 0 && !hasHist {
			return fmt.Errorf("worker %s: %s completed count %d has no response-time histogram", worker, tx, completed)
		}
		if hasHist && h.TotalCount != uint64(completed) {
			return fmt.Errorf("worker %s: %s histogram.total_count %d != completed count %d", worker, tx, h.TotalCount, completed)
		}
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
	skipSteps []string
	fileNames []string // preferred first; later names are legacy aliases
}{
	{[]string{"check_after_import"}, []string{"after-import.json"}},
	{[]string{"check_after_test", "check_after_run"}, []string{"after-test.json", "after-run.json"}},
}

func skippedStepSet(steps []string) map[string]bool {
	out := make(map[string]bool, len(steps))
	for _, s := range steps {
		out[s] = true
	}
	return out
}

func phaseSkipped(skip map[string]bool, names []string) bool {
	for _, name := range names {
		if skip[name] {
			return true
		}
	}
	return false
}

func requiredCheckFiles(skipped []string) [][]string {
	skip := skippedStepSet(skipped)
	var required [][]string
	for _, p := range checkPhaseFiles {
		if phaseSkipped(skip, p.skipSteps) {
			continue
		}
		required = append(required, p.fileNames)
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
			for _, names := range required {
				errors = append(errors, fmt.Sprintf("required check report missing: %s", names[0]))
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
	for _, names := range required {
		found := false
		for _, name := range names {
			if present[name] {
				found = true
				break
			}
		}
		if !found {
			errors = append(errors, fmt.Sprintf("required check report missing: %s", names[0]))
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
	summary := FormatSummary(agg)
	return os.WriteFile(filepath.Join(dir, "summary.txt"), []byte(summary), 0644)
}

// maxTPMCPerWarehouse is the TPC-C §A.3 theoretical maximum New-Order
// throughput per warehouse (tpmC), matching tpcc/domain/constants.h.
const maxTPMCPerWarehouse = 12.86

// summaryTxOrder matches ETransactionType in tpcc/domain/constants.h and the
// final-results dump in tpcc-postgres-cpp / tpcc/harness/run_loop.cpp.
var summaryTxOrder = []struct {
	Key  string
	Name string
}{
	{"new_order", "NewOrder"},
	{"delivery", "Delivery"},
	{"order_status", "OrderStatus"},
	{"payment", "Payment"},
	{"stock_level", "StockLevel"},
}

// FormatSummary returns a brief human-readable view of aggregate.json.
// Status flags stay as a short preamble; measurement metrics use the
// "=== TPC-C Results ===" layout from tpcc-postgres-cpp PrintFinalResults
// (with UserAborted and min/max/avg retained for portable-tpcc consolidate).
func FormatSummary(agg *Aggregate) string {
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
	appendTPCCResultsSummary(&b, agg)
	return b.String()
}

func appendTPCCResultsSummary(b *strings.Builder, agg *Aggregate) {
	meas, ok := agg.Metrics["measurement"].(map[string]interface{})
	if !ok {
		return
	}
	fmt.Fprintf(b, "=== TPC-C Results ===\n")

	warehouses := scaleWarehousesFromAggregate(agg)
	if warehouses > 0 {
		fmt.Fprintf(b, "  Scale: %d warehouses\n", warehouses)
	}

	configuredSec := measurementSecondsFromSettings(agg.Settings)
	measuredSec := configuredSec
	if v, ok := asFloat64(meas["measurement_seconds"]); ok && v > 0 {
		measuredSec = v
	} else if configuredSec <= 0 {
		if tpmc, ok := asFloat64(meas["throughput_new_order_per_min"]); ok && tpmc > 0 {
			if n, ok := asFloat64(meas["new_order_count"]); ok && n > 0 {
				measuredSec = n / tpmc * 60.0
			}
		}
	}
	if configuredSec > 0 || measuredSec > 0 {
		cfg := configuredSec
		if cfg <= 0 {
			cfg = measuredSec
		}
		fmt.Fprintf(b, "  Measured Duration: %.1fs (configured: %.0fs)\n", measuredSec, cfg)
	}

	if v, ok := asFloat64(meas["throughput_new_order_per_min"]); ok {
		fmt.Fprintf(b, "  New-Order Throughput: %.2f tpmC\n", v)
		if pacingEnabledFromSettings(agg.Settings) && warehouses > 0 {
			efficiency := v / (maxTPMCPerWarehouse * float64(warehouses)) * 100.0
			fmt.Fprintf(b, "  Efficiency: %.1f%%\n", efficiency)
		}
	}

	counters := counterMap(meas["counters"])
	totalFailed := int64(0)
	for _, tx := range summaryTxOrder {
		totalFailed += counters[tx.Key+"_failed"]
	}
	// Also count any unexpected *_failed keys.
	for k, v := range counters {
		if !strings.HasSuffix(k, "_failed") {
			continue
		}
		known := false
		for _, tx := range summaryTxOrder {
			if k == tx.Key+"_failed" {
				known = true
				break
			}
		}
		if !known {
			totalFailed += v
		}
	}
	fmt.Fprintf(b, "  Total Failed: %d\n", totalFailed)

	unit, rt := responseTimeMap(meas)
	seen := map[string]bool{}
	for _, tx := range summaryTxOrder {
		okCount := counters[tx.Key+"_ok"]
		userAborted := counters[tx.Key+"_user_aborted"]
		failed := counters[tx.Key+"_failed"]
		stats, hasStats := rt[tx.Key]
		if okCount == 0 && userAborted == 0 && failed == 0 && !hasStats {
			continue
		}
		seen[tx.Key] = true
		appendTxResultLine(b, tx.Name, unit, okCount, userAborted, failed, stats)
	}
	// Preserve any extra histogram keys not in the canonical enum order.
	extra := make([]string, 0)
	for tx := range rt {
		if !seen[tx] {
			extra = append(extra, tx)
		}
	}
	sort.Strings(extra)
	for _, tx := range extra {
		appendTxResultLine(b, txDisplayName(tx), unit,
			counters[tx+"_ok"], counters[tx+"_user_aborted"], counters[tx+"_failed"], rt[tx])
	}
}

func appendTxResultLine(
	b *strings.Builder,
	name, unit string,
	okCount, userAborted, failed int64,
	stats map[string]interface{},
) {
	fmt.Fprintf(b, "  %s: OK=%d UserAborted=%d Failed=%d", name, okCount, userAborted, failed)
	if stats != nil {
		if _, has := stats["min"]; has {
			fmt.Fprintf(b, " min=%s max=%s avg=%s",
				formatLatencyMs(stats["min"], unit),
				formatLatencyMs(stats["max"], unit),
				formatLatencyMs(stats["avg"], unit))
		}
		fmt.Fprintf(b, " p50=%s p90=%s p99=%s",
			formatLatencyMs(stats["p50"], unit),
			formatLatencyMs(stats["p90"], unit),
			formatLatencyMs(stats["p99"], unit))
	}
	b.WriteByte('\n')
}

// formatLatencyMs renders a histogram sample in milliseconds. Values recorded
// in microseconds are converted so min/max/avg and percentiles share one unit.
func formatLatencyMs(v interface{}, unit string) string {
	if v == nil {
		return "0ms"
	}
	n, ok := asFloat64(v)
	if !ok {
		return fmt.Sprintf("%vms", v)
	}
	if unit == "us" {
		n /= 1000.0
	}
	if n == math.Trunc(n) {
		return fmt.Sprintf("%.0fms", n)
	}
	return fmt.Sprintf("%.1fms", n)
}

func responseTimeMap(meas map[string]interface{}) (unit string, rt map[string]map[string]interface{}) {
	rt = map[string]map[string]interface{}{}
	unit = "ms"
	for k, v := range meas {
		if !strings.HasPrefix(k, "response_time_") {
			continue
		}
		unit = strings.TrimPrefix(k, "response_time_")
		if unit == "" {
			unit = "ms"
		}
		m, ok := v.(map[string]interface{})
		if !ok {
			continue
		}
		for tx, raw := range m {
			if stats, ok := raw.(map[string]interface{}); ok {
				rt[tx] = stats
			}
		}
		break
	}
	return unit, rt
}

func counterMap(v interface{}) map[string]int64 {
	out := map[string]int64{}
	switch m := v.(type) {
	case map[string]int64:
		for k, n := range m {
			out[k] = n
		}
	case map[string]interface{}:
		for k, raw := range m {
			if n, ok := asInt64(raw); ok {
				out[k] = n
			}
		}
	}
	return out
}

func txDisplayName(tx string) string {
	for _, known := range summaryTxOrder {
		if known.Key == tx {
			return known.Name
		}
	}
	parts := strings.Split(tx, "_")
	for i, p := range parts {
		if p == "" {
			continue
		}
		parts[i] = strings.ToUpper(p[:1]) + p[1:]
	}
	return strings.Join(parts, "")
}

func measurementSecondsFromSettings(settings map[string]interface{}) float64 {
	if settings == nil {
		return 0
	}
	switch phases := settings["phases"].(type) {
	case config.PhasesJSON:
		if phases.MeasurementMs > 0 {
			return float64(phases.MeasurementMs) / 1000.0
		}
	case map[string]interface{}:
		if ms, ok := asFloat64(phases["measurement_ms"]); ok && ms > 0 {
			return ms / 1000.0
		}
	}
	return 0
}

// scaleWarehousesFromAggregate returns the run's warehouse scale from
// consolidated measurement data (sum of worker coverage). It does not read
// settings.scale.warehouses, which may not match an overridden assignment.
func scaleWarehousesFromAggregate(agg *Aggregate) int {
	if agg == nil {
		return 0
	}
	if meas, ok := agg.Metrics["measurement"].(map[string]interface{}); ok {
		if n, ok := asInt64(meas["warehouses"]); ok && n > 0 {
			return int(n)
		}
	}
	return warehousesFromWorkerAssignment(agg.Settings)
}

func warehousesFromWorkerAssignment(settings map[string]interface{}) int {
	if settings == nil {
		return 0
	}
	raw, ok := settings["worker_assignment"]
	if !ok || raw == nil {
		return 0
	}
	total := 0
	switch assigns := raw.(type) {
	case []config.WorkerAssignmentJSON:
		for _, a := range assigns {
			total += countWarehousesInRanges(a.WarehouseRanges)
		}
	case []interface{}:
		for _, item := range assigns {
			m, ok := item.(map[string]interface{})
			if !ok {
				continue
			}
			ranges, err := parseWarehouseRanges(m["warehouse_ranges"])
			if err != nil {
				continue
			}
			total += countWarehousesInRanges(ranges)
		}
	}
	return total
}

func workerWarehouseCount(partial map[string]interface{}, expected config.WorkerAssignmentJSON) int {
	if metrics, ok := partial["metrics"].(map[string]interface{}); ok {
		if n, ok := asInt64(metrics["warehouses"]); ok && n > 0 {
			return int(n)
		}
	}
	if assign, ok := partial["assignment"].(map[string]interface{}); ok {
		if ranges, err := parseWarehouseRanges(assign["warehouse_ranges"]); err == nil {
			if n := countWarehousesInRanges(ranges); n > 0 {
				return n
			}
		}
	}
	return countWarehousesInRanges(expected.WarehouseRanges)
}

func countWarehousesInRanges(ranges [][]int) int {
	total := 0
	for _, r := range ranges {
		if len(r) != 2 || r[1] <= r[0] {
			continue
		}
		total += r[1] - r[0]
	}
	return total
}

func pacingEnabledFromSettings(settings map[string]interface{}) bool {
	if settings == nil {
		return true
	}
	switch runtime := settings["runtime"].(type) {
	case config.RunRuntime:
		if runtime.Pacing == "" {
			return true
		}
		return runtime.Pacing == "enabled"
	case map[string]interface{}:
		pacing, _ := runtime["pacing"].(string)
		if pacing == "" {
			return true
		}
		return pacing == "enabled"
	}
	return true
}

func asFloat64(v interface{}) (float64, bool) {
	switch n := v.(type) {
	case float64:
		return n, true
	case float32:
		return float64(n), true
	case int:
		return float64(n), true
	case int64:
		return float64(n), true
	case uint64:
		return float64(n), true
	case json.Number:
		f, err := n.Float64()
		return f, err == nil
	default:
		return 0, false
	}
}

func asInt64(v interface{}) (int64, bool) {
	switch n := v.(type) {
	case int64:
		return n, true
	case int:
		return int64(n), true
	case float64:
		return int64(n), true
	case uint64:
		if n > math.MaxInt64 {
			return 0, false
		}
		return int64(n), true
	case json.Number:
		i, err := n.Int64()
		return i, err == nil
	default:
		return 0, false
	}
}

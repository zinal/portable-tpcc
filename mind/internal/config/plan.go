package config

import (
	"fmt"
	"sort"
)

// WorkerArgv returns argv for launching a worker.
// When startAt is non-empty it appends --start-at=<RFC3339-UTC>.
func WorkerArgv(runConfigPath, instance, startAt string) []string {
	args := []string{
		"worker",
		"--run-config", runConfigPath,
		"--instance", instance,
	}
	if startAt != "" {
		args = append(args, "--start-at="+startAt)
	}
	return args
}

// LoaderArgv returns argv for launching a loader.
func LoaderArgv(runConfigPath, instance string) []string {
	return []string{
		"loader",
		"--run-config", runConfigPath,
		"--instance", instance,
	}
}

// SchemaArgv returns argv for the schema role.
func SchemaArgv(runConfigPath, instance string) []string {
	return []string{
		"schema",
		"--run-config", runConfigPath,
		"--instance", instance,
	}
}

// IndexesArgv returns argv for the post-load indexes role.
func IndexesArgv(runConfigPath, instance string) []string {
	return []string{
		"indexes",
		"--run-config", runConfigPath,
		"--instance", instance,
	}
}

// CleanArgv returns argv for the clean admin helper (mind-tpcc cleanup).
func CleanArgv(runConfigPath, instance string) []string {
	return []string{
		"clean",
		"--run-config", runConfigPath,
		"--instance", instance,
	}
}

// DefaultCheckConcurrencyCap limits auto-selected parallel check sessions.
const DefaultCheckConcurrencyCap = 32

// ResolveCheckConcurrency returns DBMS check sessions.
// configured > 0 is used as-is; 0 / omit is min(warehouses, DefaultCheckConcurrencyCap).
func ResolveCheckConcurrency(warehouses, configured int) int {
	if configured > 0 {
		return configured
	}
	if warehouses < 1 {
		return 1
	}
	if warehouses > DefaultCheckConcurrencyCap {
		return DefaultCheckConcurrencyCap
	}
	return warehouses
}

// EffectiveCheckConcurrency is the session count passed as --threads to check.
// cliThreads, when non-nil, overrides runtime.check_concurrency for this
// invocation and does not rewrite run-config.json.
func EffectiveCheckConcurrency(warehouses, configured int, cliThreads *int) int {
	if cliThreads != nil {
		configured = *cliThreads
	}
	return ResolveCheckConcurrency(warehouses, configured)
}

// CheckArgv returns argv for the check role.
// The after-test phase is spelled --after-run on the worker command line so
// already-deployed binaries (pre-rename) keep working; new workers accept both
// --after-test and --after-run.
func CheckArgv(runConfigPath, instance, phase string, threads int) []string {
	flag := "--after-run"
	if phase == "after-import" {
		flag = "--after-import"
	}
	argv := []string{
		"check",
		"--run-config", runConfigPath,
		"--instance", instance,
		flag,
	}
	if threads > 0 {
		argv = append(argv, fmt.Sprintf("--threads=%d", threads))
	}
	return argv
}

// PlanSnapshot describes planned operations without side effects.
type PlanSnapshot struct {
	RunID            string                 `json:"run_id"`
	ProfileName      string                 `json:"profile_name"`
	Binary           string                 `json:"binary"`
	LoadAssignment   []LoadAssignmentJSON   `json:"load_assignment"`
	WorkerAssignment []WorkerAssignmentJSON `json:"worker_assignment"`
	WorkerArgv       map[string][]string    `json:"worker_argv"`
	LoaderArgv       map[string][]string    `json:"loader_argv"`
	SchemaArgv       []string               `json:"schema_argv,omitempty"`
	IndexesArgv      []string               `json:"indexes_argv,omitempty"`
	CheckArgvImport  []string               `json:"check_argv_after_import,omitempty"`
	CheckArgvTest    []string               `json:"check_argv_after_test,omitempty"`
}

// BuildPlanSnapshot creates a plan output from run config.
// Worker argv omit --start-at; that value is computed at start time.
// checkThreads, when non-nil, overrides runtime.check_concurrency in check argv.
func BuildPlanSnapshot(rc *RunConfig, checkThreads *int) *PlanSnapshot {
	workerArgv := make(map[string][]string)
	for _, w := range rc.WorkerAssignment {
		workerArgv[w.Instance] = WorkerArgv("run-config.json", w.Instance, "")
	}
	loaderArgv := make(map[string][]string)
	for _, l := range rc.LoadAssignment {
		loaderArgv[l.Instance] = LoaderArgv("run-config.json", l.Instance)
	}
	schemaInstance := "schema-0"
	indexesInstance := "indexes-0"
	if len(rc.LoadAssignment) > 0 {
		schemaInstance = rc.LoadAssignment[0].Instance + "-schema"
		indexesInstance = rc.LoadAssignment[0].Instance + "-indexes"
	}
	threads := EffectiveCheckConcurrency(rc.Scale.Warehouses, rc.Runtime.CheckConcurrency, checkThreads)
	return &PlanSnapshot{
		RunID:            rc.RunID,
		ProfileName:      rc.ProfileName,
		Binary:           rc.Binary,
		LoadAssignment:   rc.LoadAssignment,
		WorkerAssignment: rc.WorkerAssignment,
		WorkerArgv:       workerArgv,
		LoaderArgv:       loaderArgv,
		SchemaArgv:       SchemaArgv("run-config.json", schemaInstance),
		IndexesArgv:      IndexesArgv("run-config.json", indexesInstance),
		CheckArgvImport:  CheckArgv("run-config.json", "check-0", "after-import", threads),
		CheckArgvTest:    CheckArgv("run-config.json", "check-0", "after-test", threads),
	}
}

// ExpectedWorkerNames returns sorted worker instance names from run config.
func ExpectedWorkerNames(rc *RunConfig) []string {
	names := make([]string, len(rc.WorkerAssignment))
	for i, w := range rc.WorkerAssignment {
		names[i] = w.Instance
	}
	sort.Strings(names)
	return names
}

// ValidateRunConfigAssignment verifies worker assignment completeness.
// Each warehouse in [1, scale.warehouses] must be owned by exactly one worker;
// duplicate instance names and overlapping ranges are rejected.
func ValidateRunConfigAssignment(rc *RunConfig) error {
	if len(rc.WorkerAssignment) == 0 {
		return fmt.Errorf("worker assignment is empty")
	}
	if rc.Scale.Warehouses <= 0 {
		return fmt.Errorf("scale.warehouses must be positive")
	}
	seen := make(map[string]bool, len(rc.WorkerAssignment))
	ownership := make([]string, rc.Scale.Warehouses+1)
	for _, w := range rc.WorkerAssignment {
		if w.Instance == "" {
			return fmt.Errorf("worker assignment instance must not be empty")
		}
		if seen[w.Instance] {
			return fmt.Errorf("duplicate worker instance %q", w.Instance)
		}
		seen[w.Instance] = true
		if len(w.WarehouseRanges) == 0 {
			return fmt.Errorf("worker %s has empty warehouse_ranges", w.Instance)
		}
		for _, r := range w.WarehouseRanges {
			if len(r) != 2 || r[1] <= r[0] || r[0] <= 0 {
				return fmt.Errorf("invalid warehouse range for %s", w.Instance)
			}
			if r[1] > rc.Scale.Warehouses+1 {
				return fmt.Errorf("worker %s warehouse range exceeds scale.warehouses", w.Instance)
			}
			for wh := r[0]; wh < r[1]; wh++ {
				if ownership[wh] != "" {
					return fmt.Errorf("warehouse %d assigned to both %s and %s", wh, ownership[wh], w.Instance)
				}
				ownership[wh] = w.Instance
			}
		}
	}
	for wh := 1; wh <= rc.Scale.Warehouses; wh++ {
		if ownership[wh] == "" {
			return fmt.Errorf("workers do not cover warehouse %d", wh)
		}
	}
	return nil
}

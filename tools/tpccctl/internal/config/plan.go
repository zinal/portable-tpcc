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

// CheckArgv returns argv for the check role.
func CheckArgv(runConfigPath, instance, phase string) []string {
	flag := "--after-run"
	if phase == "after-import" {
		flag = "--after-import"
	}
	return []string{
		"check",
		"--run-config", runConfigPath,
		"--instance", instance,
		flag,
	}
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
	CheckArgvImport  []string               `json:"check_argv_after_import,omitempty"`
	CheckArgvRun     []string               `json:"check_argv_after_run,omitempty"`
}

// BuildPlanSnapshot creates a plan output from run config.
// Worker argv omit --start-at; that value is computed at start time.
func BuildPlanSnapshot(rc *RunConfig) *PlanSnapshot {
	workerArgv := make(map[string][]string)
	for _, w := range rc.WorkerAssignment {
		workerArgv[w.Instance] = WorkerArgv("run-config.json", w.Instance, "")
	}
	loaderArgv := make(map[string][]string)
	for _, l := range rc.LoadAssignment {
		loaderArgv[l.Instance] = LoaderArgv("run-config.json", l.Instance)
	}
	schemaInstance := "schema-0"
	if len(rc.LoadAssignment) > 0 {
		schemaInstance = rc.LoadAssignment[0].Instance + "-schema"
	}
	return &PlanSnapshot{
		RunID:            rc.RunID,
		ProfileName:      rc.ProfileName,
		Binary:           rc.Binary,
		LoadAssignment:   rc.LoadAssignment,
		WorkerAssignment: rc.WorkerAssignment,
		WorkerArgv:       workerArgv,
		LoaderArgv:       loaderArgv,
		SchemaArgv:       SchemaArgv("run-config.json", schemaInstance),
		CheckArgvImport:  CheckArgv("run-config.json", "check-0", "after-import"),
		CheckArgvRun:     CheckArgv("run-config.json", "check-0", "after-run"),
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
func ValidateRunConfigAssignment(rc *RunConfig) error {
	if len(rc.WorkerAssignment) == 0 {
		return fmt.Errorf("worker assignment is empty")
	}
	covered := 0
	for _, w := range rc.WorkerAssignment {
		for _, r := range w.WarehouseRanges {
			if len(r) != 2 {
				return fmt.Errorf("invalid warehouse range for %s", w.Instance)
			}
			covered += r[1] - r[0]
		}
	}
	if covered != rc.Scale.Warehouses {
		return fmt.Errorf("workers cover %d warehouses, expected %d", covered, rc.Scale.Warehouses)
	}
	return nil
}

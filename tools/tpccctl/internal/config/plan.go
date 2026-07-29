package config

import (
	"fmt"
	"sort"
)

// WorkerArgv returns argv for launching a worker.
func WorkerArgv(runConfigPath, instance string) []string {
	return []string{
		"worker",
		"--run-config", runConfigPath,
		"--instance", instance,
	}
}

// LoaderArgv returns argv for launching a loader.
func LoaderArgv(runConfigPath, instance string) []string {
	return []string{
		"loader",
		"--run-config", runConfigPath,
		"--instance", instance,
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
}

// BuildPlanSnapshot creates a plan output from run config.
func BuildPlanSnapshot(rc *RunConfig) *PlanSnapshot {
	workerArgv := make(map[string][]string)
	for _, w := range rc.WorkerAssignment {
		workerArgv[w.Instance] = WorkerArgv("run-config.json", w.Instance)
	}
	loaderArgv := make(map[string][]string)
	for _, l := range rc.LoadAssignment {
		loaderArgv[l.Instance] = LoaderArgv("run-config.json", l.Instance)
	}
	return &PlanSnapshot{
		RunID:            rc.RunID,
		ProfileName:      rc.ProfileName,
		Binary:           rc.Binary,
		LoadAssignment:   rc.LoadAssignment,
		WorkerAssignment: rc.WorkerAssignment,
		WorkerArgv:       workerArgv,
		LoaderArgv:       loaderArgv,
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

package config

import (
	"fmt"
	"sort"

	"portable-tpcc/tools/tpccctl/internal/assignment"
	"portable-tpcc/tools/tpccctl/internal/canonical"
)

// LoadPlan is the load-plan.json document per specification §7.1.
type LoadPlan struct {
	SchemaVersion       int                  `json:"schema_version"`
	RunID               string               `json:"run_id"`
	PlanPayloadSHA256   string               `json:"plan_payload_sha256"`
	LoadID              string               `json:"load_id"`
	Assignments         []LoadPlanAssignment `json:"assignments"`
	Batches             []LoadPlanBatch      `json:"batches"`
}

type LoadPlanAssignment struct {
	Instance         string  `json:"instance"`
	Host             string  `json:"host"`
	WarehouseRanges  [][]int `json:"warehouse_ranges"`
	OwnsGlobalData   bool    `json:"owns_global_data"`
}

type LoadPlanBatch struct {
	BatchID              string `json:"batch_id"`
	Instance             string `json:"instance"`
	Table                string `json:"table"`
	KeyRange             []int  `json:"key_range"`
	RowCount             int    `json:"row_count"`
	BatchPayloadSHA256   string `json:"batch_payload_sha256"`
}

// BuildLoadPlan constructs load-plan.json from run config and loader assignments.
func BuildLoadPlan(runID string, loadAssign []assignment.LoaderAssignment, specStateSHA, loaderBinarySHA string) (*LoadPlan, string, error) {
	payload := map[string]interface{}{
		"assignments": toLoadPlanAssignments(loadAssign),
		"batches":     []interface{}{},
	}
	payloadSHA, err := canonical.SHA256Any(payload)
	if err != nil {
		return nil, "", err
	}

	loadIDInput := map[string]interface{}{
		"run_id":               runID,
		"plan_payload_sha256":  payloadSHA,
		"spec_state_sha256":    specStateSHA,
		"loader_binary_sha256": loaderBinarySHA,
	}
	loadID, err := canonical.SHA256(loadIDInput)
	if err != nil {
		return nil, "", err
	}

	plan := &LoadPlan{
		SchemaVersion:     1,
		RunID:             runID,
		PlanPayloadSHA256: payloadSHA,
		LoadID:            loadID,
		Assignments:       toLoadPlanAssignments(loadAssign),
		Batches:           []LoadPlanBatch{},
	}
	planSHA, err := canonical.SHA256Any(plan)
	if err != nil {
		return nil, "", err
	}
	return plan, planSHA, nil
}

func toLoadPlanAssignments(assign []assignment.LoaderAssignment) []LoadPlanAssignment {
	out := make([]LoadPlanAssignment, len(assign))
	for i, a := range assign {
		out[i] = LoadPlanAssignment{
			Instance:         a.Instance,
			Host:             a.Host,
			WarehouseRanges:  assignment.ToJSONRanges(a.WarehouseRanges),
			OwnsGlobalData:   a.OwnsGlobalData,
		}
	}
	return out
}

// WorkerArgv returns argv for launching a worker per specification §8.3.
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
	RunID            string                   `json:"run_id"`
	ProfileName      string                   `json:"profile_name"`
	LoadAssignment   []LoadAssignmentJSON     `json:"load_assignment"`
	WorkerAssignment []WorkerAssignmentJSON   `json:"worker_assignment"`
	WorkerArgv       map[string][]string      `json:"worker_argv"`
	LoaderArgv       map[string][]string      `json:"loader_argv"`
	LoadPlanSHA256   string                   `json:"load_plan_sha256"`
	RunConfigSHA256  string                   `json:"run_config_sha256"`
}

// BuildPlanSnapshot creates a plan output from run config.
func BuildPlanSnapshot(rc *RunConfig, runConfigSHA, loadPlanSHA string) *PlanSnapshot {
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
		ProfileName:      rc.Profile.Name,
		LoadAssignment:   rc.LoadAssignment,
		WorkerAssignment: rc.WorkerAssignment,
		WorkerArgv:       workerArgv,
		LoaderArgv:       loaderArgv,
		LoadPlanSHA256:   loadPlanSHA,
		RunConfigSHA256:  runConfigSHA,
	}
}

// SortedWorkerNames returns sorted worker instance names from run config.
func SortedWorkerNames(rc *RunConfig) []string {
	names := append([]string(nil), rc.ExpectedInstances.Workers...)
	sort.Strings(names)
	return names
}

// ValidateRunConfigAssignment verifies worker assignment completeness.
func ValidateRunConfigAssignment(rc *RunConfig) error {
	if len(rc.WorkerAssignment) != len(rc.ExpectedInstances.Workers) {
		return fmt.Errorf("worker assignment count mismatch")
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

package config

import (
	"fmt"
	"path/filepath"
	"sort"
	"time"

	"portable-tpcc/tools/tpccctl/internal/assignment"
	"portable-tpcc/tools/tpccctl/internal/canonical"
	"portable-tpcc/tools/tpccctl/internal/paths"
	"portable-tpcc/tools/tpccctl/internal/profile"
	"portable-tpcc/tools/tpccctl/internal/specclient"
)

// BuildInput holds artifacts needed to materialize run configuration.
type BuildInput struct {
	Profile           *profile.Profile
	ProfilePath       string
	ProfileSHA256     string
	RunID             string
	SpecDescribe      *specclient.DescribeResult
	SpecStatePath     string
	SpecStateSHA256   string
	WorkerBinary      string
	WorkerBinarySHA   string
	SpecBinary        string
	SpecBinarySHA     string
	SourceRevision    string
	CanonicalIdentity map[string]interface{}
}

// ControlConfig is the immutable control-plane configuration.
type ControlConfig struct {
	SchemaVersion int                    `json:"schema_version"`
	RunID         string                 `json:"run_id"`
	Profile       ProfileRef             `json:"profile"`
	SSH           ControlSSH             `json:"ssh"`
	Hosts         map[string]HostAddress `json:"hosts"`
	Paths         ControlPaths           `json:"paths"`
	Deploy        DeployPolicy           `json:"deploy"`
	Secrets       Secrets                `json:"secrets"`
}

type ProfileRef struct {
	Name   string `json:"name"`
	SHA256 string `json:"sha256"`
}

type ControlSSH struct {
	User             string `json:"user"`
	UseAgent         bool   `json:"use_agent"`
	KnownHosts       string `json:"known_hosts"`
	ConnectTimeoutMs int64  `json:"connect_timeout_ms"`
}

type HostAddress struct {
	Address string `json:"address"`
}

type ControlPaths struct {
	LocalArtifacts string `json:"local_artifacts"`
	RemoteRoot     string `json:"remote_root"`
	ResultRoot     string `json:"result_root"`
	StateDir       string `json:"state_dir"`
}

type DeployPolicy struct {
	Transport    string `json:"transport"`
	VerifySHA256 bool   `json:"verify_sha256"`
}

type Secrets struct {
	DatabasePasswordEnv string `json:"database_password_env"`
}

// RunConfig is the immutable runtime configuration distributed to workers.
type RunConfig struct {
	SchemaVersion    int                    `json:"schema_version"`
	RunID            string                 `json:"run_id"`
	Mode             string                 `json:"mode"`
	CreatedAt        string                 `json:"created_at"`
	Profile          ProfileRef             `json:"profile"`
	Spec             SpecBlock              `json:"spec"`
	Artifacts        Artifacts              `json:"artifacts"`
	Paths            RunPaths               `json:"paths"`
	Database         RunDatabase            `json:"database"`
	Scale            ScaleBlock             `json:"scale"`
	Load             LoadBlock              `json:"load"`
	ExpectedInstances ExpectedInstances      `json:"expected_instances"`
	Assignment       AssignmentMeta         `json:"assignment"`
	LoadAssignment   []LoadAssignmentJSON   `json:"load_assignment"`
	WorkerAssignment []WorkerAssignmentJSON `json:"worker_assignment"`
	PhasePolicy      PhasePolicy            `json:"phase_policy"`
	Runtime          RunRuntime             `json:"runtime"`
	Checks           profile.Checks         `json:"checks"`
	Collect          RunCollect             `json:"collect"`
	FailurePolicy    FailurePolicy          `json:"failure_policy"`
	Deviations       []profile.Deviation    `json:"deviations"`
}

type SpecBlock struct {
	Edition         string `json:"edition"`
	DocumentURL     string `json:"document_url"`
	DocumentSHA256  string `json:"document_sha256"`
	ModuleABI       int    `json:"module_abi"`
	ModuleSHA256    string `json:"module_sha256"`
	StateSHA256     string `json:"state_sha256"`
}

type Artifacts struct {
	WorkerBinary       string `json:"worker_binary"`
	WorkerBinarySHA256 string `json:"worker_binary_sha256"`
	SpecBinary         string `json:"spec_binary"`
	SpecBinarySHA256   string `json:"spec_binary_sha256"`
	SourceRevision     string `json:"source_revision"`
}

type RunPaths struct {
	RemoteRoot string `json:"remote_root"`
	RunDir     string `json:"run_dir"`
	SpecState  string `json:"spec_state"`
	LoadPlan   string `json:"load_plan"`
}

type RunDatabase struct {
	DBMS              string                 `json:"dbms"`
	Endpoint          string                 `json:"endpoint"`
	Database          string                 `json:"database"`
	Path              string                 `json:"path"`
	CanonicalIdentity map[string]interface{} `json:"canonical_identity"`
	PasswordEnv       string                 `json:"password_env"`
	Options           map[string]interface{} `json:"options"`
}

type ScaleBlock struct {
	Warehouses int `json:"warehouses"`
}

type LoadBlock struct {
	LoadID         string `json:"load_id"`
	PlanSHA256     string `json:"plan_sha256"`
	BatchRows      int    `json:"batch_rows"`
	WriteContract  string `json:"write_contract"`
}

type ExpectedInstances struct {
	Loaders          []string `json:"loaders"`
	Workers          []string `json:"workers"`
	WorkersSHA256    string   `json:"workers_sha256"`
}

type AssignmentMeta struct {
	Algorithm      string `json:"algorithm"`
	Source         string `json:"source"`
	SortKey        string `json:"sort_key"`
	ManualOverride bool   `json:"manual_override"`
}

type LoadAssignmentJSON struct {
	Instance         string  `json:"instance"`
	Host             string  `json:"host"`
	WarehouseRanges  [][]int `json:"warehouse_ranges"`
	OwnsGlobalData   bool    `json:"owns_global_data"`
}

type WorkerAssignmentJSON struct {
	Instance         string  `json:"instance"`
	Host             string  `json:"host"`
	WarehouseRanges  [][]int `json:"warehouse_ranges"`
	Threads          int     `json:"threads"`
	MaxInflight      int     `json:"max_inflight"`
}

type PhasePolicy struct {
	StartLeadMs            int64  `json:"start_lead_ms"`
	RampUpMs               int64  `json:"ramp_up_ms"`
	MeasurementMs          int64  `json:"measurement_ms"`
	TransactionDrainMs     int64  `json:"transaction_drain_ms"`
	AsyncWorkDrain         string `json:"async_work_drain"`
	StopGraceMs            int64  `json:"stop_grace_ms"`
	MaxClockSkewMs         int64  `json:"max_clock_skew_ms"`
	MaxClockUncertaintyMs  int64  `json:"max_clock_uncertainty_ms"`
	MaxClockDriftMs        int64  `json:"max_clock_drift_ms"`
}

type RunRuntime struct {
	Pacing    string         `json:"pacing"`
	Retry     RetryJSON      `json:"retry"`
	Histogram HistogramJSON  `json:"histogram"`
}

type RetryJSON struct {
	MaxAttempts          int    `json:"max_attempts"`
	InitialBackoffMs     int64  `json:"initial_backoff_ms"`
	MaxBackoffMs         int64  `json:"max_backoff_ms"`
	Jitter               string `json:"jitter"`
	RetryAmbiguousCommit bool   `json:"retry_ambiguous_commit"`
}

type HistogramJSON struct {
	Encoding           string `json:"encoding"`
	Unit               string `json:"unit"`
	Lowest             int64  `json:"lowest"`
	Highest            int64  `json:"highest"`
	SignificantFigures int    `json:"significant_figures"`
}

type RunCollect struct {
	IncludeEvents          bool `json:"include_events"`
	IncludeLogs            bool `json:"include_logs"`
	RequireSealedArtifacts bool `json:"require_sealed_artifacts"`
}

type FailurePolicy struct {
	MissingWorker      string `json:"missing_worker"`
	LostDatabaseFence  string `json:"lost_database_fence"`
	UnsealedArtifact   string `json:"unsealed_artifact"`
}

// BuildControlConfig materializes control-config.json from profile and paths.
func BuildControlConfig(in BuildInput, expandedPaths ExpandedPaths) (*ControlConfig, error) {
	connectMs, err := profile.ParseDurationMs(in.Profile.SSH.ConnectTimeout)
	if err != nil {
		return nil, err
	}
	hosts := make(map[string]HostAddress)
	for name, h := range in.Profile.Hosts {
		if name == "control" {
			continue
		}
		hosts[name] = HostAddress{Address: h.Address}
	}
	return &ControlConfig{
		SchemaVersion: 1,
		RunID:         in.RunID,
		Profile:       ProfileRef{Name: in.Profile.Metadata.Name, SHA256: in.ProfileSHA256},
		SSH: ControlSSH{
			User:             in.Profile.SSH.User,
			UseAgent:         in.Profile.SSH.UseAgent,
			KnownHosts:       expandedPaths.KnownHostsResolved(in.Profile.SSH.KnownHosts),
			ConnectTimeoutMs: connectMs,
		},
		Hosts: hosts,
		Paths: ControlPaths{
			LocalArtifacts: expandedPaths.LocalArtifacts,
			RemoteRoot:     expandedPaths.RemoteRoot,
			ResultRoot:     expandedPaths.ResultRoot,
			StateDir:       expandedPaths.StateDir,
		},
		Deploy: DeployPolicy{Transport: "tar-over-ssh", VerifySHA256: true},
		Secrets: Secrets{DatabasePasswordEnv: in.Profile.Database.PasswordEnv},
	}, nil
}

// ExpandedPaths holds resolved filesystem paths from profile.
type ExpandedPaths struct {
	LocalArtifacts string
	RemoteRoot     string
	ResultRoot     string
	StateDir       string
	KnownHosts     string
}

// ExpandProfilePaths resolves ~ in profile path fields.
func ExpandProfilePaths(p *profile.Profile) (ExpandedPaths, error) {
	local, err := paths.ExpandHome(p.Paths.LocalArtifacts)
	if err != nil {
		return ExpandedPaths{}, err
	}
	result, err := paths.ExpandHome(p.Paths.ResultRoot)
	if err != nil {
		return ExpandedPaths{}, err
	}
	state, err := paths.ExpandHome(p.Paths.StateDir)
	if err != nil {
		return ExpandedPaths{}, err
	}
	known, err := paths.ExpandHome(p.SSH.KnownHosts)
	if err != nil {
		return ExpandedPaths{}, err
	}
	return ExpandedPaths{
		LocalArtifacts: local,
		RemoteRoot:     p.Paths.RemoteRoot,
		ResultRoot:     result,
		StateDir:       state,
		KnownHosts:     known,
	}, nil
}

func (e ExpandedPaths) KnownHostsResolved(fallback string) string {
	if e.KnownHosts != "" {
		return e.KnownHosts
	}
	return fallback
}

// BuildRunConfig materializes run-config.json.
func BuildRunConfig(in BuildInput, ep ExpandedPaths, loadPlanSHA string, loadID string) (*RunConfig, error) {
	p := in.Profile
	loadAssign, err := assignment.BuildLoaderAssignments(p.LoaderInstances(), p.Scale.Warehouses)
	if err != nil {
		return nil, err
	}
	workerAssign, err := assignment.BuildWorkerAssignments(
		p.WorkerInstances(),
		p.Scale.Warehouses,
		p.Runtime.ThreadsPerWorker,
		p.Runtime.MaxInflightPerWorker,
	)
	if err != nil {
		return nil, err
	}

	startLead, err := profile.ParseDurationMs(p.Phases.StartLead)
	if err != nil {
		return nil, err
	}
	rampUp, err := profile.ParseDurationMs(p.Phases.RampUp)
	if err != nil {
		return nil, err
	}
	measurement, err := profile.ParseDurationMs(p.Phases.Measurement)
	if err != nil {
		return nil, err
	}
	drain, err := profile.ParseDurationMs(p.Phases.TransactionDrain)
	if err != nil {
		return nil, err
	}
	stopGrace, err := profile.ParseDurationMs(p.Phases.StopGrace)
	if err != nil {
		return nil, err
	}
	skew, err := profile.ParseDurationMs(p.Phases.MaxClockSkew)
	if err != nil {
		return nil, err
	}
	uncertainty, err := profile.ParseDurationMs(p.Phases.MaxClockUncertainty)
	if err != nil {
		return nil, err
	}
	drift, err := profile.ParseDurationMs(p.Phases.MaxClockDrift)
	if err != nil {
		return nil, err
	}
	initBackoff, err := profile.ParseDurationMs(p.Runtime.Retry.InitialBackoff)
	if err != nil {
		return nil, err
	}
	maxBackoff, err := profile.ParseDurationMs(p.Runtime.Retry.MaxBackoff)
	if err != nil {
		return nil, err
	}

	runDir := filepath.Join(p.Paths.RemoteRoot, "runs", in.RunID)
	specStatePath := filepath.Join(runDir, "spec-state.json")
	loadPlanPath := filepath.Join(runDir, "load-plan.json")

	loaderNames := make([]string, len(p.Loaders))
	for i, l := range p.Loaders {
		loaderNames[i] = l.Name
	}
	workerNames := make([]string, len(p.Workers))
	for i, w := range p.Workers {
		workerNames[i] = w.Name
	}
	sort.Strings(workerNames)
	workersSHA, err := canonical.SHA256(map[string]interface{}{
		"workers": workerNames,
	})
	if err != nil {
		return nil, err
	}

	desc := in.SpecDescribe
	if desc == nil {
		desc = &specclient.DescribeResult{
			Edition:        p.Spec.Edition,
			DocumentURL:    "",
			DocumentSHA256: "",
			ModuleABI:      1,
			ModuleSHA256:   "",
		}
	}

	identity := in.CanonicalIdentity
	if identity == nil {
		identity = map[string]interface{}{
			"dbms":     p.Database.DBMS,
			"endpoint": p.Database.Endpoint,
			"database": p.Database.Database,
			"path":     p.Database.Path,
		}
	}

	workerBinaryName := in.WorkerBinary
	if workerBinaryName == "" {
		workerBinaryName = fmt.Sprintf("tpcc-%s", p.Database.DBMS)
	}

	rc := &RunConfig{
		SchemaVersion: 1,
		RunID:         in.RunID,
		Mode:          p.Mode,
		CreatedAt:     time.Now().UTC().Format(time.RFC3339),
		Profile:       ProfileRef{Name: p.Metadata.Name, SHA256: in.ProfileSHA256},
		Spec: SpecBlock{
			Edition:        desc.Edition,
			DocumentURL:    desc.DocumentURL,
			DocumentSHA256: desc.DocumentSHA256,
			ModuleABI:      desc.ModuleABI,
			ModuleSHA256:   desc.ModuleSHA256,
			StateSHA256:    in.SpecStateSHA256,
		},
		Artifacts: Artifacts{
			WorkerBinary:       workerBinaryName,
			WorkerBinarySHA256: in.WorkerBinarySHA,
			SpecBinary:         in.SpecBinary,
			SpecBinarySHA256:   in.SpecBinarySHA,
			SourceRevision:     in.SourceRevision,
		},
		Paths: RunPaths{
			RemoteRoot: p.Paths.RemoteRoot,
			RunDir:     runDir,
			SpecState:  specStatePath,
			LoadPlan:   loadPlanPath,
		},
		Database: RunDatabase{
			DBMS:              p.Database.DBMS,
			Endpoint:          p.Database.Endpoint,
			Database:          p.Database.Database,
			Path:              p.Database.Path,
			CanonicalIdentity: identity,
			PasswordEnv:       p.Database.PasswordEnv,
			Options:           p.Database.Options,
		},
		Scale: ScaleBlock{Warehouses: p.Scale.Warehouses},
		Load: LoadBlock{
			LoadID:        loadID,
			PlanSHA256:    loadPlanSHA,
			BatchRows:     p.Data.BatchRows,
			WriteContract: "idempotent-put-batch-v1",
		},
		ExpectedInstances: ExpectedInstances{
			Loaders:       loaderNames,
			Workers:       workerNames,
			WorkersSHA256: workersSHA,
		},
		Assignment: AssignmentMeta{
			Algorithm:      assignment.AlgorithmBalancedContiguousV1,
			Source:         "generated",
			SortKey:        "instance_name",
			ManualOverride: false,
		},
		LoadAssignment:   toLoadJSON(loadAssign),
		WorkerAssignment: toWorkerJSON(workerAssign),
		PhasePolicy: PhasePolicy{
			StartLeadMs:           startLead,
			RampUpMs:              rampUp,
			MeasurementMs:         measurement,
			TransactionDrainMs:    drain,
			AsyncWorkDrain:        "from_spec_state",
			StopGraceMs:           stopGrace,
			MaxClockSkewMs:        skew,
			MaxClockUncertaintyMs: uncertainty,
			MaxClockDriftMs:       drift,
		},
		Runtime: RunRuntime{
			Pacing: p.Runtime.Pacing,
			Retry: RetryJSON{
				MaxAttempts:          p.Runtime.Retry.MaxAttempts,
				InitialBackoffMs:     initBackoff,
				MaxBackoffMs:         maxBackoff,
				Jitter:               p.Runtime.Retry.Jitter,
				RetryAmbiguousCommit: false,
			},
			Histogram: HistogramJSON{
				Encoding:           "hdr-v1",
				Unit:               p.Runtime.Histogram.Unit,
				Lowest:             p.Runtime.Histogram.Lowest,
				Highest:            p.Runtime.Histogram.Highest,
				SignificantFigures: p.Runtime.Histogram.SignificantFigures,
			},
		},
		Checks:  p.Checks,
		Collect: RunCollect{
			IncludeEvents:          p.Collect.IncludeEvents,
			IncludeLogs:            p.Collect.IncludeLogs,
			RequireSealedArtifacts: true,
		},
		FailurePolicy: FailurePolicy{
			MissingWorker:     "fail",
			LostDatabaseFence: "fail",
			UnsealedArtifact:  "fail",
		},
		Deviations: p.Deviations,
	}
	return rc, nil
}

func toLoadJSON(assign []assignment.LoaderAssignment) []LoadAssignmentJSON {
	out := make([]LoadAssignmentJSON, len(assign))
	for i, a := range assign {
		out[i] = LoadAssignmentJSON{
			Instance:         a.Instance,
			Host:             a.Host,
			WarehouseRanges:  assignment.ToJSONRanges(a.WarehouseRanges),
			OwnsGlobalData:   a.OwnsGlobalData,
		}
	}
	return out
}

func toWorkerJSON(assign []assignment.WorkerAssignment) []WorkerAssignmentJSON {
	out := make([]WorkerAssignmentJSON, len(assign))
	for i, a := range assign {
		out[i] = WorkerAssignmentJSON{
			Instance:         a.Instance,
			Host:             a.Host,
			WarehouseRanges:  assignment.ToJSONRanges(a.WarehouseRanges),
			Threads:          a.Threads,
			MaxInflight:      a.MaxInflight,
		}
	}
	return out
}

// GenerateRunID creates a run identifier from profile metadata.
func GenerateRunID(profileName string) string {
	now := time.Now().UTC()
	return fmt.Sprintf("%s-%s-01", now.Format("20060102"), profileName)
}

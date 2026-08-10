package config

import (
	"fmt"
	"path/filepath"
	"time"

	"portable-tpcc/mind/internal/assignment"
	"portable-tpcc/mind/internal/paths"
	"portable-tpcc/mind/internal/profile"
)

// BuildInput holds artifacts needed to materialize run configuration.
type BuildInput struct {
	Profile      *profile.Profile
	ProfilePath  string
	RunID        string
	WorkerBinary string
}

// RunConfig is the immutable runtime configuration distributed to loaders and workers.
// It carries concrete settings (specification §5), not hash stand-ins.
type RunConfig struct {
	SchemaVersion    int                    `json:"schema_version"`
	RunID            string                 `json:"run_id"`
	CreatedAt        string                 `json:"created_at"`
	ProfileName      string                 `json:"profile_name"`
	Binary           string                 `json:"binary"`
	Database         RunDatabase            `json:"database"`
	Scale            ScaleBlock             `json:"scale"`
	Data             DataBlock              `json:"data"`
	Workload         WorkloadBlock          `json:"workload"`
	LoadAssignment   []LoadAssignmentJSON   `json:"load_assignment"`
	WorkerAssignment []WorkerAssignmentJSON `json:"worker_assignment"`
	Phases           PhasesJSON             `json:"phases"`
	Runtime          RunRuntime             `json:"runtime"`
}

// Remote credential filenames placed next to run-config.json on worker hosts.
const (
	RemoteCAFileName    = "ca.pem"
	RemoteSAKeyFileName = "sa-key.json"
)

type RunDatabase struct {
	DBMS        string                 `json:"dbms"`
	Endpoint    string                 `json:"endpoint"`
	Database    string                 `json:"database"`
	Path        string                 `json:"path"`
	AuthScheme  string                 `json:"auth_scheme,omitempty"`
	User        string                 `json:"user,omitempty"`
	PasswordEnv string                 `json:"password_env,omitempty"`
	SaKeyFile   string                 `json:"sa_key_file,omitempty"`
	CaFile      string                 `json:"ca_file,omitempty"`
	Options     map[string]interface{} `json:"options,omitempty"`
}

type ScaleBlock struct {
	Warehouses int `json:"warehouses"`
}

type DataBlock struct {
	Seed      int64 `json:"seed,omitempty"`
	BatchRows int   `json:"batch_rows"`
}

type WorkloadBlock struct {
	TerminalsPerWarehouse int                `json:"terminals_per_warehouse"`
	TransactionMix        TransactionMixJSON `json:"transaction_mix"`
	KeyingTimeMs          TxTimingJSON       `json:"keying_time_ms"`
	ThinkTimeMs           TxTimingJSON       `json:"think_time_ms"`
}

type TransactionMixJSON struct {
	NewOrder    int `json:"new_order"`
	Payment     int `json:"payment"`
	OrderStatus int `json:"order_status"`
	Delivery    int `json:"delivery"`
	StockLevel  int `json:"stock_level"`
}

type TxTimingJSON struct {
	NewOrder    int `json:"new_order"`
	Payment     int `json:"payment"`
	OrderStatus int `json:"order_status"`
	Delivery    int `json:"delivery"`
	StockLevel  int `json:"stock_level"`
}

type LoadAssignmentJSON struct {
	Instance        string  `json:"instance"`
	Host            string  `json:"host"`
	WarehouseRanges [][]int `json:"warehouse_ranges"`
	OwnsGlobalData  bool    `json:"owns_global_data"`
}

type WorkerAssignmentJSON struct {
	Instance        string  `json:"instance"`
	Host            string  `json:"host"`
	WarehouseRanges [][]int `json:"warehouse_ranges"`
	Threads         int     `json:"threads"`
	MaxInflight     int     `json:"max_inflight"`
}

type PhasesJSON struct {
	StartLeadMs        int64 `json:"start_lead_ms"`
	RampUpMs           int64 `json:"ramp_up_ms"`
	MeasurementMs      int64 `json:"measurement_ms"`
	TransactionDrainMs int64 `json:"transaction_drain_ms"`
	AsyncWorkDrainMs   int64 `json:"async_work_drain_ms"`
	StopGraceMs        int64 `json:"stop_grace_ms"`
	MaxClockSkewMs     int64 `json:"max_clock_skew_ms"`
}

type RunRuntime struct {
	Pacing                string        `json:"pacing"`
	ThinkTimeDistribution string        `json:"think_time_distribution"`
	Retry                 RetryJSON     `json:"retry"`
	Histogram             HistogramJSON `json:"histogram"`
}

type RetryJSON struct {
	MaxAttempts          int    `json:"max_attempts"`
	InitialBackoffMs     int64  `json:"initial_backoff_ms"`
	MaxBackoffMs         int64  `json:"max_backoff_ms"`
	Jitter               string `json:"jitter"`
	RetryAmbiguousCommit bool   `json:"retry_ambiguous_commit"`
}

// HistogramJSON is the materialized runtime.histogram block.
// Matches THistogram linear_exp: unit + highest (max_value); hdr_till is
// derived by the worker and published in result artifacts.
type HistogramJSON struct {
	Unit    string `json:"unit"`
	Highest int64  `json:"highest"`
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
	remoteRoot, err := paths.ExpandHome(p.Paths.RemoteRoot)
	if err != nil {
		return ExpandedPaths{}, err
	}
	if abs, err := filepath.Abs(remoteRoot); err == nil {
		remoteRoot = abs
	}
	return ExpandedPaths{
		LocalArtifacts: local,
		RemoteRoot:     remoteRoot,
		ResultRoot:     result,
		StateDir:       state,
		KnownHosts:     known,
	}, nil
}

// BuildRunConfig materializes run-config.json from the profile and defaults.
func BuildRunConfig(in BuildInput) (*RunConfig, error) {
	p := in.Profile
	loadAssign, err := assignment.BuildLoaderAssignments(p.LoaderInstances(), p.Scale.Warehouses)
	if err != nil {
		return nil, err
	}
	threads := p.Runtime.ThreadsPerWorker
	if threads <= 0 {
		threads = 1
	}
	maxInflight := p.Runtime.MaxInflightPerWorker
	if maxInflight <= 0 {
		maxInflight = 64
	}
	workerAssign, err := assignment.BuildWorkerAssignments(
		p.WorkerInstances(),
		p.Scale.Warehouses,
		threads,
		maxInflight,
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
	asyncDrain := drain
	if p.Phases.AsyncWorkDrain != "" {
		asyncDrain, err = profile.ParseDurationMs(p.Phases.AsyncWorkDrain)
		if err != nil {
			return nil, err
		}
	}
	stopGrace, err := profile.ParseDurationMs(p.Phases.StopGrace)
	if err != nil {
		return nil, err
	}
	skew, err := profile.ParseDurationMs(p.Phases.MaxClockSkew)
	if err != nil {
		return nil, err
	}

	retry := DefaultRetry()
	if p.Runtime.Retry.MaxAttempts > 0 {
		retry.MaxAttempts = p.Runtime.Retry.MaxAttempts
	}
	if p.Runtime.Retry.InitialBackoff != "" {
		initBackoff, err := profile.ParseDurationMs(p.Runtime.Retry.InitialBackoff)
		if err != nil {
			return nil, err
		}
		retry.InitialBackoffMs = initBackoff
	}
	if p.Runtime.Retry.MaxBackoff != "" {
		maxBackoff, err := profile.ParseDurationMs(p.Runtime.Retry.MaxBackoff)
		if err != nil {
			return nil, err
		}
		retry.MaxBackoffMs = maxBackoff
	}
	if p.Runtime.Retry.Jitter != "" {
		retry.Jitter = p.Runtime.Retry.Jitter
	}

	hist := DefaultHistogram()
	if p.Runtime.Histogram.Unit != "" {
		hist.Unit = p.Runtime.Histogram.Unit
	}
	if p.Runtime.Histogram.Highest > 0 {
		hist.Highest = p.Runtime.Histogram.Highest
	}

	pacing := p.Runtime.Pacing
	if pacing == "" {
		pacing = "enabled"
	}
	thinkDist := ResolveThinkTimeDistribution(p.Runtime.ThinkTimeDistribution)

	binary := in.WorkerBinary
	if binary == "" {
		binary = fmt.Sprintf("tpcc-%s", p.Database.DBMS)
	} else {
		binary = filepath.Base(binary)
	}

	batchRows := p.Data.BatchRows
	if batchRows <= 0 {
		batchRows = 10000
	}

	data := DataBlock{BatchRows: batchRows}
	if p.Data.Seed != nil {
		data.Seed = *p.Data.Seed
	}

	db := RunDatabase{
		DBMS:        p.Database.DBMS,
		Endpoint:    p.Database.Endpoint,
		Database:    p.Database.Database,
		Path:        p.Database.Path,
		AuthScheme:  p.Database.AuthScheme,
		User:        p.Database.User,
		PasswordEnv: p.Database.PasswordEnv,
		Options:     p.Database.Options,
	}
	// Rewrite control-host credential paths to fixed names under the remote
	// run directory. Orchestrator deploy uploads the local files there.
	if p.Database.CaFile != "" {
		db.CaFile = RemoteCAFileName
	}
	if p.Database.SaKeyFile != "" {
		db.SaKeyFile = RemoteSAKeyFileName
	}
	if db.AuthScheme == "" && p.Database.DBMS == "ydb" {
		db.AuthScheme = InferYdbAuthScheme(p.Database)
	}

	return &RunConfig{
		SchemaVersion:    1,
		RunID:            in.RunID,
		CreatedAt:        time.Now().UTC().Format(time.RFC3339),
		ProfileName:      p.Metadata.Name,
		Binary:           binary,
		Database:         db,
		Scale:            ScaleBlock{Warehouses: p.Scale.Warehouses},
		Data:             data,
		Workload:         ResolveWorkload(p.Workload),
		LoadAssignment:   toLoadJSON(loadAssign),
		WorkerAssignment: toWorkerJSON(workerAssign),
		Phases: PhasesJSON{
			StartLeadMs:        startLead,
			RampUpMs:           rampUp,
			MeasurementMs:      measurement,
			TransactionDrainMs: drain,
			AsyncWorkDrainMs:   asyncDrain,
			StopGraceMs:        stopGrace,
			MaxClockSkewMs:     skew,
		},
		Runtime: RunRuntime{
			Pacing:                pacing,
			ThinkTimeDistribution: thinkDist,
			Retry:                 retry,
			Histogram:             hist,
		},
	}, nil
}

// InferYdbAuthScheme picks anonymous/login/sa_key from populated profile fields.
func InferYdbAuthScheme(db profile.Database) string {
	if db.AuthScheme != "" {
		return db.AuthScheme
	}
	if db.SaKeyFile != "" {
		return "sa_key"
	}
	if db.PasswordEnv != "" || db.User != "" {
		return "login"
	}
	return "anonymous"
}

func toLoadJSON(assign []assignment.LoaderAssignment) []LoadAssignmentJSON {
	out := make([]LoadAssignmentJSON, len(assign))
	for i, a := range assign {
		out[i] = LoadAssignmentJSON{
			Instance:        a.Instance,
			Host:            a.Host,
			WarehouseRanges: assignment.ToJSONRanges(a.WarehouseRanges),
			OwnsGlobalData:  a.OwnsGlobalData,
		}
	}
	return out
}

func toWorkerJSON(assign []assignment.WorkerAssignment) []WorkerAssignmentJSON {
	out := make([]WorkerAssignmentJSON, len(assign))
	for i, a := range assign {
		out[i] = WorkerAssignmentJSON{
			Instance:        a.Instance,
			Host:            a.Host,
			WarehouseRanges: assignment.ToJSONRanges(a.WarehouseRanges),
			Threads:         a.Threads,
			MaxInflight:     a.MaxInflight,
		}
	}
	return out
}

// GenerateRunID creates a run identifier from profile metadata.
func GenerateRunID(profileName string) string {
	now := time.Now().UTC()
	return fmt.Sprintf("%s-%s-01", now.Format("20060102"), profileName)
}

// SettingsForAggregate returns concrete settings for aggregate.json (no secrets).
func SettingsForAggregate(rc *RunConfig) map[string]interface{} {
	db := map[string]interface{}{
		"dbms":     rc.Database.DBMS,
		"endpoint": rc.Database.Endpoint,
		"database": rc.Database.Database,
		"path":     rc.Database.Path,
		"options":  rc.Database.Options,
	}
	if rc.Database.AuthScheme != "" {
		db["auth_scheme"] = rc.Database.AuthScheme
	}
	if rc.Database.User != "" {
		db["user"] = rc.Database.User
	}
	if rc.Database.CaFile != "" {
		db["ca_file"] = rc.Database.CaFile
	}
	if rc.Database.SaKeyFile != "" {
		db["sa_key_file"] = rc.Database.SaKeyFile
	}
	return map[string]interface{}{
		"profile_name":      rc.ProfileName,
		"binary":            rc.Binary,
		"database":          db,
		"scale":             rc.Scale,
		"data":              rc.Data,
		"workload":          rc.Workload,
		"load_assignment":   rc.LoadAssignment,
		"worker_assignment": rc.WorkerAssignment,
		"phases":            rc.Phases,
		"runtime":           rc.Runtime,
	}
}

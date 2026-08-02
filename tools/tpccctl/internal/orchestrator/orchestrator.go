package orchestrator

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"portable-tpcc/tools/tpccctl/internal/canonical"
	"portable-tpcc/tools/tpccctl/internal/config"
	"portable-tpcc/tools/tpccctl/internal/consolidate"
	"portable-tpcc/tools/tpccctl/internal/deploy"
	"portable-tpcc/tools/tpccctl/internal/profile"
	"portable-tpcc/tools/tpccctl/internal/redact"
	"portable-tpcc/tools/tpccctl/internal/schedule"
	"portable-tpcc/tools/tpccctl/internal/state"
	"portable-tpcc/tools/tpccctl/internal/validate"
)

// Options configure the orchestrator runtime.
type Options struct {
	ProfilePath  string
	RunID        string
	WorkerBinary string
	SkipSteps    []string
}

// Orchestrator coordinates tpccctl stages.
type Orchestrator struct {
	Opts       Options
	Profile    *profile.Profile
	Expanded   config.ExpandedPaths
	StateStore *state.Store
}

// Context holds materialized configuration for a run.
type Context struct {
	RunID     string
	RunConfig *config.RunConfig
	RunDir    string
}

// New creates an orchestrator from a profile path.
func New(opts Options) (*Orchestrator, error) {
	p, err := profile.ParseFile(opts.ProfilePath)
	if err != nil {
		return nil, err
	}
	ep, err := config.ExpandProfilePaths(p)
	if err != nil {
		return nil, err
	}
	store := &state.Store{StateDir: ep.StateDir}
	return &Orchestrator{
		Opts:       opts,
		Profile:    p,
		Expanded:   ep,
		StateStore: store,
	}, nil
}

// Validate runs profile validation without side effects.
func (o *Orchestrator) Validate() *validate.Result {
	return validate.Profile(o.Profile)
}

// Materialize builds run configuration artifacts on the control host.
// When run-config.json already exists for the run, it is reused unchanged so
// worker run_config_sha256 stays valid across later stages (collect/consolidate).
func (o *Orchestrator) Materialize() (*Context, error) {
	vr := o.Validate()
	if !vr.Valid {
		return nil, fmt.Errorf("profile invalid: %v", vr.Errors)
	}
	runID := o.Opts.RunID
	if runID == "" {
		runID = config.GenerateRunID(o.Profile.Metadata.Name)
	}
	profileBytes, err := os.ReadFile(o.Opts.ProfilePath)
	if err != nil {
		return nil, err
	}

	runDir := o.StateStore.RunDir(runID)
	if err := os.MkdirAll(runDir, 0755); err != nil {
		return nil, err
	}

	runConfigPath := filepath.Join(runDir, "run-config.json")
	var rc *config.RunConfig
	if data, err := os.ReadFile(runConfigPath); err == nil {
		rc = &config.RunConfig{}
		if err := json.Unmarshal(data, rc); err != nil {
			return nil, fmt.Errorf("load existing run-config.json: %w", err)
		}
		if rc.RunID != "" && rc.RunID != runID {
			return nil, fmt.Errorf("existing run-config run_id %q does not match %q", rc.RunID, runID)
		}
		rc.RunID = runID
	} else if os.IsNotExist(err) {
		rc, err = config.BuildRunConfig(config.BuildInput{
			Profile:      o.Profile,
			ProfilePath:  o.Opts.ProfilePath,
			RunID:        runID,
			WorkerBinary: o.Opts.WorkerBinary,
		})
		if err != nil {
			return nil, err
		}
		if err := state.WriteJSON(runDir, "run-config.json", rc); err != nil {
			return nil, err
		}
	} else {
		return nil, err
	}

	redacted, err := redact.RedactYAML(profileBytes)
	if err != nil {
		return nil, err
	}
	if err := os.WriteFile(filepath.Join(runDir, "profile.redacted.yaml"), redacted, 0644); err != nil {
		return nil, err
	}

	rs, err := o.StateStore.Load(runID)
	if err != nil {
		return nil, err
	}
	rs.State = state.StatePlanned
	rs.InsecureHostKey = o.Profile.SSH.InsecureIgnore
	if err := o.StateStore.Save(rs); err != nil {
		return nil, err
	}

	return &Context{
		RunID:     runID,
		RunConfig: rc,
		RunDir:    runDir,
	}, nil
}

// Plan returns a plan snapshot.
func (o *Orchestrator) Plan() (*config.PlanSnapshot, error) {
	ctx, err := o.Materialize()
	if err != nil {
		return nil, err
	}
	return config.BuildPlanSnapshot(ctx.RunConfig), nil
}

// Deploy distributes binary + run-config.json to runtime hosts.
func (o *Orchestrator) Deploy(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StateDeploying); err != nil {
		return err
	}
	sessions, err := o.openSessions()
	if err != nil {
		o.StateStore.Fail(ctx.RunID, err)
		return err
	}
	defer closeSessions(sessions)

	if err := o.deployToHosts(ctx, sessions); err != nil {
		o.StateStore.Fail(ctx.RunID, err)
		return err
	}
	// Keep local-mode deploy manifest for cleanup of shared artifact trees when used.
	if abs, err := filepath.Abs(o.Expanded.RemoteRoot); err == nil {
		ld := &deploy.LocalDeploy{
			SourceRoot: o.Expanded.LocalArtifacts,
			TargetRoot: abs,
		}
		_, _ = ld.Deploy(o.Expanded.LocalArtifacts, false)
	}
	return o.StateStore.Transition(ctx.RunID, state.StateSchema)
}

// Run executes the full pipeline (specification §9).
func (o *Orchestrator) Run() error {
	ctx, err := o.Materialize()
	if err != nil {
		return err
	}
	if err := o.StateStore.AcquireProfileLock(o.Profile.Metadata.Name, ctx.RunID); err != nil {
		return err
	}
	defer o.StateStore.ReleaseProfileLock(o.Profile.Metadata.Name, ctx.RunID)

	steps := []struct {
		name string
		fn   func() error
	}{
		{"deploy", func() error { return o.Deploy(ctx) }},
		{"schema", func() error { return o.schema(ctx) }},
		{"load", func() error { return o.load(ctx) }},
		{"check_after_import", func() error { return o.check(ctx, "after-import") }},
		{"start", func() error { return o.start(ctx) }},
		{"check_after_run", func() error { return o.check(ctx, "after-run") }},
		{"collect", func() error { return o.collect(ctx) }},
		{"consolidate", func() error { return o.consolidate(ctx) }},
	}
	for _, step := range steps {
		if o.shouldSkip(step.name) {
			_ = o.StateStore.RecordSkip(ctx.RunID, step.name)
			continue
		}
		if err := step.fn(); err != nil {
			o.StateStore.Fail(ctx.RunID, err)
			return fmt.Errorf("%s: %w", step.name, err)
		}
	}
	return o.StateStore.Transition(ctx.RunID, state.StateCompleted)
}

func (o *Orchestrator) shouldSkip(name string) bool {
	for _, s := range o.Opts.SkipSteps {
		if s == name {
			return true
		}
	}
	return false
}

func (o *Orchestrator) schema(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StateSchema); err != nil {
		return err
	}
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)

	hostKey := ctx.RunConfig.LoadAssignment[0].Host
	instance := "schema-0"
	argv := config.SchemaArgv("run-config.json", instance)
	proc, err := o.launchRole(ctx, sessions, "schema", hostKey, instance, argv)
	if err != nil {
		return err
	}
	// Schema is synchronous enough: wait up to 30m for completion.
	return o.waitProcesses(ctx, []*launchedProc{proc}, 30*time.Minute, true)
}

// RunSchema applies database schema via the remote schema role.
func (o *Orchestrator) RunSchema(ctx *Context) error {
	return o.schema(ctx)
}

func (o *Orchestrator) load(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StateLoading); err != nil {
		return err
	}
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)

	var procs []*launchedProc
	for _, l := range ctx.RunConfig.LoadAssignment {
		argv := config.LoaderArgv("run-config.json", l.Instance)
		proc, err := o.launchRole(ctx, sessions, "loader", l.Host, l.Instance, argv)
		if err != nil {
			_ = o.stopPeers(ctx, sessions)
			return err
		}
		procs = append(procs, proc)
	}
	if err := o.waitProcesses(ctx, procs, 12*time.Hour, true); err != nil {
		_ = o.stopPeers(ctx, sessions)
		return err
	}
	return o.StateStore.Transition(ctx.RunID, state.StateCheckingImport)
}

// RunLoad runs horizontal loaders.
func (o *Orchestrator) RunLoad(ctx *Context) error {
	return o.load(ctx)
}

func (o *Orchestrator) check(ctx *Context, phase string) error {
	if phase == "after-import" {
		if err := o.StateStore.Transition(ctx.RunID, state.StateCheckingImport); err != nil {
			return err
		}
		if !o.Profile.Checks.AfterImport && !containsSkipOverride(o.Opts.SkipSteps, "check_after_import") {
			// Profile may disable; still allow explicit check command.
		}
	} else {
		if err := o.StateStore.Transition(ctx.RunID, state.StateCheckingResult); err != nil {
			return err
		}
	}
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)

	hostKey := ctx.RunConfig.LoadAssignment[0].Host
	instance := "check-0"
	argv := config.CheckArgv("run-config.json", instance, phase)
	proc, err := o.launchRole(ctx, sessions, "check", hostKey, instance, argv)
	if err != nil {
		return err
	}
	if err := o.waitProcesses(ctx, []*launchedProc{proc}, 2*time.Hour, true); err != nil {
		if o.Profile.Checks.FailFast {
			return err
		}
		return err
	}
	return nil
}

func containsSkipOverride(steps []string, name string) bool {
	for _, s := range steps {
		if s == name {
			return true
		}
	}
	return false
}

// RunCheck records check phase and executes the check role.
func (o *Orchestrator) RunCheck(ctx *Context, phase string) error {
	return o.check(ctx, phase)
}

func (o *Orchestrator) start(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StatePreparing); err != nil {
		return err
	}
	warnTPCSettingsDeviations(ctx)
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)

	token := schedule.Compute(ctx.RunConfig, time.Now().UTC())
	if err := state.WriteJSON(ctx.RunDir, "start-token.json", token); err != nil {
		return err
	}
	rs, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		return err
	}
	rs.StartAt = token.StartAt
	if err := o.StateStore.Save(rs); err != nil {
		return err
	}

	var workers []*launchedProc
	for _, w := range ctx.RunConfig.WorkerAssignment {
		argv := config.WorkerArgv("run-config.json", w.Instance, token.StartAt)
		proc, err := o.launchRole(ctx, sessions, "worker", w.Host, w.Instance, argv)
		if err != nil {
			_ = o.stopPeers(ctx, sessions)
			return err
		}
		workers = append(workers, proc)
	}
	return o.superviseWorkers(ctx, workers, token, sessions)
}

// RunStart arms workers with a shared --start-at and supervises phases.
func (o *Orchestrator) RunStart(ctx *Context) error {
	return o.start(ctx)
}

func (o *Orchestrator) collect(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StateCollecting); err != nil {
		return err
	}
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)
	return o.collectArtifacts(ctx, sessions)
}

// RunCollect pulls artifacts into results/<run_id>/.
func (o *Orchestrator) RunCollect(ctx *Context) error {
	return o.collect(ctx)
}

func (o *Orchestrator) consolidate(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StateConsolidating); err != nil {
		return err
	}
	rs, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		return err
	}
	runConfigSHA, err := canonical.SHA256File(filepath.Join(ctx.RunDir, "run-config.json"))
	if err != nil {
		return fmt.Errorf("hash run-config.json for consolidate: %w", err)
	}
	cons := &consolidate.Consolidator{ResultRoot: o.Expanded.ResultRoot}
	agg, err := cons.ConsolidateWithOptions(ctx.RunID, ctx.RunConfig, consolidate.Options{
		SkippedSteps:            rs.SkippedSteps,
		MaxClockSkewMs:          ctx.RunConfig.Phases.MaxClockSkewMs,
		ExpectedRunConfigSHA256: runConfigSHA,
	})
	if err != nil {
		return err
	}
	return consolidate.WriteAggregate(o.Expanded.ResultRoot, ctx.RunID, agg)
}

// RunConsolidate merges artifacts into aggregate.json.
func (o *Orchestrator) RunConsolidate(ctx *Context) error {
	return o.consolidate(ctx)
}

// Status returns current run state.
func (o *Orchestrator) Status(runID string) (*state.RunState, error) {
	if runID == "" || runID == "latest" {
		latest, err := o.StateStore.LatestRunID()
		if err != nil {
			return nil, err
		}
		if latest == "" {
			return nil, fmt.Errorf("no runs found under %s", o.StateStore.StateDir)
		}
		runID = latest
	}
	return o.StateStore.Load(runID)
}

// Stop terminates running processes for a run.
func (o *Orchestrator) Stop(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StateStopping); err != nil {
		return err
	}
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)
	return o.stopPeers(ctx, sessions)
}

// Cleanup deploy artifacts.
func (o *Orchestrator) Cleanup(yes bool) error {
	return deploy.Cleanup(o.Expanded.RemoteRoot, yes)
}

// WritePlanJSON encodes plan snapshot to JSON.
func WritePlanJSON(plan *config.PlanSnapshot) ([]byte, error) {
	return json.MarshalIndent(plan, "", "  ")
}

func warnTPCSettingsDeviations(ctx *Context) {
	devs := config.TPCSettingsDeviations(ctx.RunConfig)
	if len(devs) == 0 {
		return
	}
	msg := "WARNING: effective run settings deviate from TPC-C 5.11 launch-parameter requirements; result_class remains engineering"
	fmt.Fprintln(os.Stderr, msg)
	var b strings.Builder
	b.WriteString(msg)
	b.WriteByte('\n')
	for _, d := range devs {
		line := "WARNING: tpcc_settings_deviation: " + d
		fmt.Fprintln(os.Stderr, line)
		b.WriteString(line)
		b.WriteByte('\n')
	}
	_ = os.WriteFile(filepath.Join(ctx.RunDir, "orchestrator.log"), []byte(b.String()), 0644)
}

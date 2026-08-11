package orchestrator

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"portable-tpcc/mind/internal/canonical"
	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/consolidate"
	"portable-tpcc/mind/internal/deploy"
	"portable-tpcc/mind/internal/paths"
	"portable-tpcc/mind/internal/profile"
	"portable-tpcc/mind/internal/progress"
	"portable-tpcc/mind/internal/redact"
	"portable-tpcc/mind/internal/remote"
	"portable-tpcc/mind/internal/schedule"
	"portable-tpcc/mind/internal/state"
	"portable-tpcc/mind/internal/validate"
)

// Options configure the orchestrator runtime.
type Options struct {
	ProfilePath  string
	RunID        string
	WorkerBinary string
	SkipSteps    []string
}

// Orchestrator coordinates mind-tpcc stages.
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
	runID, err := o.ResolveRunID()
	if err != nil {
		return nil, err
	}
	profileBytes, err := os.ReadFile(o.Opts.ProfilePath)
	if err != nil {
		return nil, err
	}
	profileSHA := canonical.SHA256Bytes(profileBytes)

	runDir := o.StateStore.RunDir(runID)
	if err := os.MkdirAll(runDir, 0755); err != nil {
		return nil, err
	}

	runConfigPath := filepath.Join(runDir, "run-config.json")
	profileSHAPath := filepath.Join(runDir, "profile.sha256")
	profileRedactedPath := filepath.Join(runDir, "profile.redacted.yaml")
	var rc *config.RunConfig
	created := false
	if data, err := os.ReadFile(runConfigPath); err == nil {
		rc = &config.RunConfig{}
		if err := json.Unmarshal(data, rc); err != nil {
			return nil, fmt.Errorf("load existing run-config.json: %w", err)
		}
		if rc.RunID != "" && rc.RunID != runID {
			return nil, fmt.Errorf("existing run-config run_id %q does not match %q", rc.RunID, runID)
		}
		storedSHA, err := os.ReadFile(profileSHAPath)
		if err != nil {
			if os.IsNotExist(err) {
				return nil, fmt.Errorf("existing run %s is missing profile.sha256", runID)
			}
			return nil, err
		}
		if strings.TrimSpace(string(storedSHA)) != profileSHA {
			return nil, fmt.Errorf("existing run %s was created from a different profile", runID)
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
		if err := os.WriteFile(profileSHAPath, []byte(profileSHA+"\n"), 0644); err != nil {
			return nil, err
		}
		created = true
	} else {
		return nil, err
	}

	if created || !fileExists(profileRedactedPath) {
		redacted, err := redact.RedactYAML(profileBytes)
		if err != nil {
			return nil, err
		}
		if err := os.WriteFile(profileRedactedPath, redacted, 0644); err != nil {
			return nil, err
		}
	}

	_, stateErr := os.Stat(o.StateStore.StatePath(runID))
	stateExists := stateErr == nil
	if stateErr != nil && !os.IsNotExist(stateErr) {
		return nil, stateErr
	}
	rs, err := o.StateStore.Load(runID)
	if err != nil {
		return nil, err
	}
	if !stateExists || rs.State == "" || rs.State == state.StatePlanned {
		rs.State = state.StatePlanned
	}
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

// ResolveRunID computes the run ID without creating run artifacts.
// When --run-id is omitted, continues the latest non-terminal run created from
// this profile so staged commands (deploy → schema → load → …) share one run.
func (o *Orchestrator) ResolveRunID() (string, error) {
	vr := o.Validate()
	if !vr.Valid {
		return "", fmt.Errorf("profile invalid: %v", vr.Errors)
	}
	runID := o.Opts.RunID
	if runID == "" {
		if latest, err := o.latestContinuableRunID(); err != nil {
			return "", err
		} else if latest != "" {
			progress.Printf("continuing run_id=%s", latest)
			return latest, nil
		}
		var err error
		runID, err = o.uniqueRunID(config.GenerateRunID(o.Profile.Metadata.Name))
		if err != nil {
			return "", err
		}
	}
	return runID, nil
}

// latestContinuableRunID returns the newest non-terminal run for this profile.
func (o *Orchestrator) latestContinuableRunID() (string, error) {
	latest, err := o.StateStore.LatestRunID()
	if err != nil || latest == "" {
		return latest, err
	}
	rs, err := o.StateStore.Load(latest)
	if err != nil {
		return "", err
	}
	if state.IsTerminal(rs.State) {
		return "", nil
	}
	profileBytes, err := os.ReadFile(o.Opts.ProfilePath)
	if err != nil {
		return "", err
	}
	wantSHA := canonical.SHA256Bytes(profileBytes)
	stored, err := os.ReadFile(filepath.Join(o.StateStore.RunDir(latest), "profile.sha256"))
	if err != nil {
		if os.IsNotExist(err) {
			return "", nil
		}
		return "", err
	}
	if strings.TrimSpace(string(stored)) != wantSHA {
		return "", nil
	}
	return latest, nil
}

func (o *Orchestrator) uniqueRunID(first string) (string, error) {
	stem := strings.TrimSuffix(first, "-01")
	for i := 1; i < 100; i++ {
		runID := fmt.Sprintf("%s-%02d", stem, i)
		exists, err := o.runArtifactsExist(runID)
		if err != nil {
			return "", err
		}
		if !exists {
			return runID, nil
		}
	}
	return "", fmt.Errorf("no free run_id suffix for %s", stem)
}

func (o *Orchestrator) runArtifactsExist(runID string) (bool, error) {
	for _, path := range []string{
		o.StateStore.RunDir(runID),
		filepath.Join(o.Expanded.ResultRoot, runID),
	} {
		if _, err := os.Stat(path); err == nil {
			return true, nil
		} else if !os.IsNotExist(err) {
			return false, err
		}
	}
	return false, nil
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
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
	progress.Printf("stage deploy: start (run_id=%s)", ctx.RunID)
	rs, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		return err
	}
	// Re-deploy into an in-progress staged run is allowed; only enter the
	// deploying state from planned/deploying so later stages are not reset.
	if rs.State == "" || rs.State == state.StatePlanned || rs.State == state.StateDeploying {
		if err := o.StateStore.Transition(ctx.RunID, state.StateDeploying); err != nil {
			return err
		}
	} else {
		progress.Printf("redeploying into existing run (state=%s)", rs.State)
	}
	sessions, err := o.openSessions()
	if err != nil {
		o.StateStore.Fail(ctx.RunID, err)
		return err
	}
	closed := false
	defer func() {
		if !closed {
			closeSessions(sessions)
		}
	}()

	if err := o.deployToHosts(ctx, sessions); err != nil {
		o.StateStore.Fail(ctx.RunID, err)
		return err
	}
	progress.Printf("closing runtime sessions")
	closeSessions(sessions)
	closed = true

	// LocalDeploy writes deploy-manifest.json under the control-host view of
	// remote_root for cleanup of shared local artifact trees. Skip it for
	// pure SSH runs: it copies+hashes local_artifacts into a local path and
	// can stall deploy for tens of seconds with no effect on remote hosts.
	if usesLocalRuntime(o.Profile) {
		if err := o.writeLocalDeployManifest(); err != nil {
			progress.Printf("local deploy manifest: %v", err)
		}
	}
	if deployShouldMarkSchema(rs.State) {
		if err := o.StateStore.Transition(ctx.RunID, state.StateSchema); err != nil {
			return err
		}
	}
	progress.Printf("stage deploy: complete")
	return nil
}

func deployShouldMarkSchema(st string) bool {
	switch st {
	case "", state.StatePlanned, state.StateDeploying, state.StateSchema:
		return true
	default:
		return false
	}
}

// usesLocalRuntime reports whether any loader/worker host uses a Local session.
func usesLocalRuntime(p *profile.Profile) bool {
	for _, host := range remote.UniqueHosts(p) {
		if remote.IsLoopback(host) {
			return true
		}
	}
	return false
}

func (o *Orchestrator) writeLocalDeployManifest() error {
	localRoot, err := paths.ExpandHome(o.Expanded.RemoteRoot)
	if err != nil {
		return err
	}
	abs, err := filepath.Abs(localRoot)
	if err != nil {
		return err
	}
	progress.Printf("writing local deploy manifest under %s", abs)
	ld := &deploy.LocalDeploy{
		SourceRoot: o.Expanded.LocalArtifacts,
		TargetRoot: abs,
	}
	_, err = ld.Deploy(o.Expanded.LocalArtifacts, false)
	return err
}

// Run executes the full pipeline (specification §9).
func (o *Orchestrator) Run() error {
	runID, err := o.ResolveRunID()
	if err != nil {
		return err
	}
	if err := o.StateStore.AcquireProfileLock(o.Profile.Metadata.Name, runID); err != nil {
		return err
	}
	defer o.StateStore.ReleaseProfileLock(o.Profile.Metadata.Name, runID)
	oldRunID := o.Opts.RunID
	o.Opts.RunID = runID
	defer func() { o.Opts.RunID = oldRunID }()
	progress.Printf("run %s: materializing configuration", runID)
	ctx, err := o.Materialize()
	if err != nil {
		return err
	}
	progress.Printf("run %s: starting pipeline", runID)

	steps := []struct {
		name    string
		enabled bool
		fn      func() error
	}{
		{"deploy", true, func() error { return o.Deploy(ctx) }},
		{"schema", true, func() error { return o.schema(ctx) }},
		{"load", true, func() error { return o.load(ctx) }},
		{"check_after_import", o.Profile.Checks.AfterImport, func() error { return o.check(ctx, "after-import") }},
		{"start", true, func() error { return o.start(ctx) }},
		{"check_after_run", o.Profile.Checks.AfterRun, func() error { return o.check(ctx, "after-run") }},
		{"collect", true, func() error { return o.collect(ctx) }},
		{"consolidate", true, func() error { return o.consolidate(ctx) }},
	}
	for _, step := range steps {
		if !step.enabled || o.shouldSkip(step.name) {
			progress.Printf("run %s: skip step %s", runID, step.name)
			_ = o.StateStore.RecordSkip(ctx.RunID, step.name)
			continue
		}
		progress.Printf("run %s: step %s", runID, step.name)
		if err := step.fn(); err != nil {
			if isCheckStep(step.name) && !o.Profile.Checks.FailFast {
				progress.Printf("run %s: step %s failed (fail_fast=false): %v", runID, step.name, err)
				continue
			}
			o.StateStore.Fail(ctx.RunID, err)
			return fmt.Errorf("%s: %w", step.name, err)
		}
		progress.Printf("run %s: step %s complete", runID, step.name)
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateCompleted); err != nil {
		return err
	}
	progress.Printf("run %s: completed", runID)
	return nil
}

func (o *Orchestrator) shouldSkip(name string) bool {
	for _, s := range o.Opts.SkipSteps {
		if s == name {
			return true
		}
	}
	return false
}

func isCheckStep(name string) bool {
	return name == "check_after_import" || name == "check_after_run"
}

func (o *Orchestrator) schema(ctx *Context) error {
	progress.Printf("stage schema: start (run_id=%s)", ctx.RunID)
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
	if err := o.waitProcesses(ctx, []*launchedProc{proc}, 30*time.Minute, true); err != nil {
		_ = o.stopPeers(ctx, sessions)
		return err
	}
	progress.Printf("stage schema: complete")
	return nil
}

// RunSchema applies database schema via the remote schema role.
func (o *Orchestrator) RunSchema(ctx *Context) error {
	return o.schema(ctx)
}

func (o *Orchestrator) load(ctx *Context) error {
	progress.Printf("stage load: start (%d loader(s), run_id=%s)", len(ctx.RunConfig.LoadAssignment), ctx.RunID)
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
	if err := o.StateStore.Transition(ctx.RunID, state.StateCheckingImport); err != nil {
		return err
	}
	progress.Printf("stage load: complete")
	return nil
}

// RunLoad runs horizontal loaders.
func (o *Orchestrator) RunLoad(ctx *Context) error {
	return o.load(ctx)
}

func (o *Orchestrator) check(ctx *Context, phase string) error {
	progress.Printf("stage check (%s): start (run_id=%s)", phase, ctx.RunID)
	if phase == "after-import" {
		if err := o.StateStore.Transition(ctx.RunID, state.StateCheckingImport); err != nil {
			return err
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
		_ = o.stopPeers(ctx, sessions)
		return err
	}
	progress.Printf("stage check (%s): complete", phase)
	return nil
}

// RunCheck records check phase and executes the check role.
func (o *Orchestrator) RunCheck(ctx *Context, phase string) error {
	return o.check(ctx, phase)
}

func (o *Orchestrator) start(ctx *Context) error {
	progress.Printf("stage start: start (%d worker(s), run_id=%s)", len(ctx.RunConfig.WorkerAssignment), ctx.RunID)
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
	progress.Printf("stage start: start-at %s", token.StartAt)
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
	if err := o.superviseWorkers(ctx, workers, token, sessions); err != nil {
		return err
	}
	progress.Printf("stage start: complete")
	return nil
}

// RunStart arms workers with a shared --start-at and supervises phases.
func (o *Orchestrator) RunStart(ctx *Context) error {
	return o.start(ctx)
}

func (o *Orchestrator) collect(ctx *Context) error {
	progress.Printf("stage collect: start (run_id=%s)", ctx.RunID)
	if err := o.StateStore.Transition(ctx.RunID, state.StateCollecting); err != nil {
		return err
	}
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)
	if err := o.collectArtifacts(ctx, sessions); err != nil {
		return err
	}
	progress.Printf("stage collect: complete")
	return nil
}

// RunCollect pulls artifacts into results/<run_id>/.
func (o *Orchestrator) RunCollect(ctx *Context) error {
	return o.collect(ctx)
}

func (o *Orchestrator) consolidate(ctx *Context) error {
	progress.Printf("stage consolidate: start (run_id=%s)", ctx.RunID)
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
	if err := consolidate.WriteAggregate(o.Expanded.ResultRoot, ctx.RunID, agg); err != nil {
		return err
	}
	progress.Printf("stage consolidate: complete (aggregate.json written)")
	return nil
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
	progress.Printf("stage stop: start (run_id=%s)", ctx.RunID)
	if err := o.StateStore.Transition(ctx.RunID, state.StateStopping); err != nil {
		return err
	}
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)
	if err := o.stopPeers(ctx, sessions); err != nil {
		return err
	}
	progress.Printf("stage stop: complete")
	return nil
}

// Cleanup deploy artifacts.
func (o *Orchestrator) Cleanup(yes bool) error {
	root, err := paths.ExpandHome(o.Expanded.RemoteRoot)
	if err != nil {
		return err
	}
	if abs, err := filepath.Abs(root); err == nil {
		root = abs
	}
	progress.Printf("cleanup: removing deploy artifacts under %s", root)
	if err := deploy.Cleanup(root, yes); err != nil {
		return err
	}
	progress.Printf("cleanup: complete")
	return nil
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

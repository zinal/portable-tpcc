package orchestrator

import (
	"context"
	"encoding/json"
	"errors"
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

// ErrInterrupted is returned when Options.Interrupt is cancelled (e.g. Ctrl+C).
var ErrInterrupted = errors.New("interrupted")

// Options configure the orchestrator runtime.
type Options struct {
	ProfilePath  string
	RunID        string
	WorkerBinary string
	SkipSteps    []string
	Overrides    config.ProfileOverrides
	// Interrupt, when cancelled, aborts long waits so callers can release the
	// profile lock via defer (SIGINT/SIGTERM). Nil means not interruptible.
	Interrupt context.Context
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
	if err := config.ApplyOverrides(p, opts.Overrides); err != nil {
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
		if err := config.OverridesMatchRunConfig(o.Opts.Overrides, rc); err != nil {
			return nil, fmt.Errorf("existing run %s: %w", runID, err)
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
// this profile so staged commands (schema → load → …) share one run.
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
// When CLI overrides disagree with that run's run-config (e.g. a second
// `start --warehouses N` after a completed measurement left the run in
// draining), the run is not continued so Materialize allocates a new id.
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
	if o.Opts.Overrides.Any() {
		rcPath := filepath.Join(o.StateStore.RunDir(latest), "run-config.json")
		data, err := os.ReadFile(rcPath)
		if err != nil {
			if os.IsNotExist(err) {
				return latest, nil
			}
			return "", err
		}
		rc := &config.RunConfig{}
		if err := json.Unmarshal(data, rc); err != nil {
			return "", fmt.Errorf("load existing run-config.json: %w", err)
		}
		if err := config.OverridesMatchRunConfig(o.Opts.Overrides, rc); err != nil {
			progress.Printf("overrides differ from run_id=%s; allocating a new run", latest)
			return "", nil
		}
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

// Deploy uploads the shared worker binary to runtime hosts.
// It is profile-scoped: it does not allocate a run_id, materialize run-config,
// or mutate per-run FSM state. Per-run run-config.json is uploaded at launch.
func (o *Orchestrator) Deploy() error {
	vr := o.Validate()
	if !vr.Valid {
		return fmt.Errorf("profile invalid: %v", vr.Errors)
	}
	progress.Printf("deploy: start (profile=%s, shared binary)", o.Profile.Metadata.Name)
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)

	if err := o.deployToHosts(sessions); err != nil {
		return err
	}

	// LocalDeploy writes deploy-manifest.json under the control-host view of
	// remote_root for cleanup of shared local artifact trees. Skip it for
	// pure SSH runs: it copies+hashes local_artifacts into a local path and
	// can stall deploy for tens of seconds with no effect on remote hosts.
	if usesLocalRuntime(o.Profile) {
		if err := o.writeLocalDeployManifest(); err != nil {
			progress.Printf("local deploy manifest: %v", err)
		}
	}
	progress.Printf("deploy: complete")
	return nil
}

// requireWorkerBinary verifies that an explicit `mind-tpcc deploy` already
// placed the shared worker binary on every runtime host. It does not upload
// artifacts: run must not silently refresh binaries (version skew risk).
func (o *Orchestrator) requireWorkerBinary() error {
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)

	binName, missing, err := o.workerBinaryMissingHosts(sessions)
	if err != nil {
		return err
	}
	if len(missing) == 0 {
		progress.Printf("shared worker binary %s present on all hosts", binName)
		return nil
	}
	return fmt.Errorf(
		"shared worker binary %s missing on host(s) %s; run `mind-tpcc deploy --profile ...` first (and after any binary rebuild)",
		binName, strings.Join(missing, ", "),
	)
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
		{"deploy", true, func() error { return o.requireWorkerBinary() }},
		{"schema", true, func() error { return o.schema(ctx) }},
		{"load", true, func() error { return o.load(ctx) }},
		{"indexes", true, func() error { return o.indexes(ctx) }},
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
		progress.Printf(
			"load shard %s on %s: ranges=%v owns_global_data=%v threads=%d",
			l.Instance, l.Host, l.WarehouseRanges, l.OwnsGlobalData, l.Threads,
		)
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
	progress.Printf("stage load: complete")
	return nil
}

// RunLoad runs horizontal loaders.
func (o *Orchestrator) RunLoad(ctx *Context) error {
	return o.load(ctx)
}

func (o *Orchestrator) indexes(ctx *Context) error {
	progress.Printf("stage indexes: start (run_id=%s)", ctx.RunID)
	if err := o.StateStore.Transition(ctx.RunID, state.StateIndexing); err != nil {
		return err
	}
	sessions, err := o.openSessions()
	if err != nil {
		return err
	}
	defer closeSessions(sessions)

	hostKey := ctx.RunConfig.LoadAssignment[0].Host
	instance := "indexes-0"
	argv := config.IndexesArgv("run-config.json", instance)
	proc, err := o.launchRole(ctx, sessions, "indexes", hostKey, instance, argv)
	if err != nil {
		return err
	}
	if err := o.waitProcesses(ctx, []*launchedProc{proc}, 12*time.Hour, true); err != nil {
		_ = o.stopPeers(ctx, sessions)
		return err
	}
	progress.Printf("stage indexes: complete")
	return nil
}

// RunIndexes creates secondary indexes and gathers statistics after load.
func (o *Orchestrator) RunIndexes(ctx *Context) error {
	return o.indexes(ctx)
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

// ResolveCleanupRunID selects an existing run for cleanup.
// Explicit --run-id wins; otherwise the newest run for this profile is used
// (including terminal runs). Does not allocate a new run_id.
func (o *Orchestrator) ResolveCleanupRunID() (string, error) {
	vr := o.Validate()
	if !vr.Valid {
		return "", fmt.Errorf("profile invalid: %v", vr.Errors)
	}
	runID := strings.TrimSpace(o.Opts.RunID)
	if runID == "" {
		latest, err := o.latestCleanupRunID()
		if err != nil {
			return "", err
		}
		if latest == "" {
			return "", fmt.Errorf("no runs found for profile %q under %s", o.Profile.Metadata.Name, o.StateStore.StateDir)
		}
		progress.Printf("cleanup run_id=%s", latest)
		return latest, nil
	}
	if err := validateCleanupRunID(runID); err != nil {
		return "", err
	}
	if err := o.verifyRunProfileSHA(runID); err != nil {
		return "", err
	}
	progress.Printf("cleanup run_id=%s", runID)
	return runID, nil
}

func validateCleanupRunID(runID string) error {
	if runID == "." || runID == ".." {
		return fmt.Errorf("invalid run_id %q", runID)
	}
	if strings.Contains(runID, "/") || strings.Contains(runID, `\`) || strings.Contains(runID, "..") {
		return fmt.Errorf("invalid run_id %q", runID)
	}
	return nil
}

func (o *Orchestrator) latestCleanupRunID() (string, error) {
	root := filepath.Join(o.StateStore.StateDir, "runs")
	entries, err := os.ReadDir(root)
	if err != nil {
		if os.IsNotExist(err) {
			return "", nil
		}
		return "", err
	}
	profileBytes, err := os.ReadFile(o.Opts.ProfilePath)
	if err != nil {
		return "", err
	}
	wantSHA := canonical.SHA256Bytes(profileBytes)
	var bestID string
	var bestTime time.Time
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		runID := e.Name()
		stored, err := os.ReadFile(filepath.Join(o.StateStore.RunDir(runID), "profile.sha256"))
		if err != nil || strings.TrimSpace(string(stored)) != wantSHA {
			continue
		}
		if _, err := os.Stat(filepath.Join(o.StateStore.RunDir(runID), "run-config.json")); err != nil {
			continue
		}
		rs, err := o.StateStore.Load(runID)
		if err != nil {
			continue
		}
		t, err := time.Parse(time.RFC3339, rs.UpdatedAt)
		if err != nil {
			info, ierr := e.Info()
			if ierr != nil {
				continue
			}
			t = info.ModTime()
		}
		if bestID == "" || t.After(bestTime) {
			bestID = runID
			bestTime = t
		}
	}
	return bestID, nil
}

func (o *Orchestrator) verifyRunProfileSHA(runID string) error {
	runDir := o.StateStore.RunDir(runID)
	if _, err := os.Stat(filepath.Join(runDir, "run-config.json")); err != nil {
		if os.IsNotExist(err) {
			return fmt.Errorf("run %s not found under %s", runID, o.StateStore.StateDir)
		}
		return err
	}
	profileBytes, err := os.ReadFile(o.Opts.ProfilePath)
	if err != nil {
		return err
	}
	wantSHA := canonical.SHA256Bytes(profileBytes)
	stored, err := os.ReadFile(filepath.Join(runDir, "profile.sha256"))
	if err != nil {
		if os.IsNotExist(err) {
			return fmt.Errorf("run %s is missing profile.sha256", runID)
		}
		return err
	}
	if strings.TrimSpace(string(stored)) != wantSHA {
		return fmt.Errorf("run %s was created from a different profile", runID)
	}
	return nil
}

// LoadExistingContext loads an already materialized run without creating artifacts.
func (o *Orchestrator) LoadExistingContext(runID string) (*Context, error) {
	if err := o.verifyRunProfileSHA(runID); err != nil {
		return nil, err
	}
	runDir := o.StateStore.RunDir(runID)
	data, err := os.ReadFile(filepath.Join(runDir, "run-config.json"))
	if err != nil {
		return nil, err
	}
	rc := &config.RunConfig{}
	if err := json.Unmarshal(data, rc); err != nil {
		return nil, fmt.Errorf("load run-config.json: %w", err)
	}
	if rc.RunID != "" && rc.RunID != runID {
		return nil, fmt.Errorf("run-config run_id %q does not match %q", rc.RunID, runID)
	}
	rc.RunID = runID
	return &Context{RunID: runID, RunConfig: rc, RunDir: runDir}, nil
}

func cleanupNeedsRemote(st string) bool {
	switch st {
	case "", state.StatePlanned:
		return false
	default:
		return true
	}
}

func cleanupNeedsDB(st string) bool {
	switch st {
	case "", state.StatePlanned, state.StateDeploying:
		return false
	default:
		return true
	}
}

func hasRunningProcesses(rs *state.RunState) bool {
	for _, p := range rs.Processes {
		if p.State == "running" && p.PID > 0 {
			return true
		}
	}
	return false
}

// Cleanup tears down a run according to its state: stop peers, drop TPC-C
// objects when schema may exist, remove remote_root/<run_id> on every host,
// then delete local results and state for the run.
func (o *Orchestrator) Cleanup(ctx *Context, yes bool) error {
	if !yes {
		return fmt.Errorf("cleanup requires --yes")
	}
	rs, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		return err
	}
	progress.Printf("cleanup: start (run_id=%s, state=%s)", ctx.RunID, rs.State)

	needRemote := cleanupNeedsRemote(rs.State)
	needDB := cleanupNeedsDB(rs.State)
	needSessions := needRemote || needDB || hasRunningProcesses(rs)

	if needSessions {
		sessions, err := o.openSessions()
		if err != nil {
			return err
		}
		defer closeSessions(sessions)

		if hasRunningProcesses(rs) {
			progress.Printf("cleanup: stopping running processes")
			if err := o.stopPeers(ctx, sessions); err != nil {
				return err
			}
		}
		if needDB {
			if err := o.cleanupDatabase(ctx, sessions); err != nil {
				return err
			}
		} else {
			progress.Printf("cleanup: skip database clean (state=%s)", rs.State)
		}
		if needRemote {
			if err := o.removeRemoteRunDirs(ctx, sessions); err != nil {
				return err
			}
		} else {
			progress.Printf("cleanup: skip remote run dirs (state=%s)", rs.State)
		}
	} else {
		progress.Printf("cleanup: no remote sessions needed (state=%s)", rs.State)
	}

	if err := o.removeLocalResults(ctx.RunID); err != nil {
		return err
	}
	if err := o.cleanupLocalDeployManifest(); err != nil {
		return err
	}
	if err := o.removeLocalRunState(ctx.RunID); err != nil {
		return err
	}
	progress.Printf("cleanup: complete")
	return nil
}

func (o *Orchestrator) cleanupDatabase(ctx *Context, sessions map[string]remote.Session) error {
	if len(ctx.RunConfig.LoadAssignment) == 0 {
		return fmt.Errorf("cleanup: run-config has no load_assignment host for database clean")
	}
	hostKey := ctx.RunConfig.LoadAssignment[0].Host
	sess, ok := sessions[hostKey]
	if !ok {
		return fmt.Errorf("cleanup: no session for host %s", hostKey)
	}
	remoteBin, err := o.sessionBinaryPath(sess, ctx.RunConfig.Binary)
	if err != nil {
		return err
	}
	exists, err := sess.Exists(remoteBin)
	if err != nil {
		return err
	}
	if !exists {
		progress.Printf("cleanup: skip database clean (worker binary missing on %s)", hostKey)
		return nil
	}
	instance := "clean-0"
	argv := config.CleanArgv("run-config.json", instance)
	progress.Printf("cleanup: drop TPC-C objects via %s/%s", hostKey, instance)
	proc, err := o.launchRole(ctx, sessions, "clean", hostKey, instance, argv)
	if err != nil {
		return err
	}
	if err := o.waitProcesses(ctx, []*launchedProc{proc}, 30*time.Minute, true); err != nil {
		return err
	}
	progress.Printf("cleanup: database clean finished")
	return nil
}

func (o *Orchestrator) removeRemoteRunDirs(ctx *Context, sessions map[string]remote.Session) error {
	for hostKey, sess := range sessions {
		runDir, err := o.sessionRunDir(sess, ctx.RunID)
		if err != nil {
			return fmt.Errorf("cleanup host %s: %w", hostKey, err)
		}
		if err := assertSafeRemoteRunDir(runDir, ctx.RunID); err != nil {
			return fmt.Errorf("cleanup host %s: %w", hostKey, err)
		}
		exists, err := sess.Exists(runDir)
		if err != nil {
			return fmt.Errorf("cleanup host %s: %w", hostKey, err)
		}
		if !exists {
			progress.Printf("cleanup %s: remote run dir absent (%s)", hostKey, runDir)
			continue
		}
		progress.Printf("cleanup %s: remove %s", hostKey, runDir)
		if err := sess.RemoveAll(runDir); err != nil {
			return fmt.Errorf("cleanup host %s remove %s: %w", hostKey, runDir, err)
		}
	}
	return nil
}

func assertSafeRemoteRunDir(runDir, runID string) error {
	clean := filepath.Clean(runDir)
	if clean == "" || clean == "." || clean == "/" || clean == string(filepath.Separator) {
		return fmt.Errorf("refusing to remove unsafe path %q", runDir)
	}
	base := filepath.Base(clean)
	if base != runID {
		return fmt.Errorf("refusing to remove %q: base %q is not run_id %q", runDir, base, runID)
	}
	return nil
}

func (o *Orchestrator) removeLocalResults(runID string) error {
	dir := filepath.Join(o.Expanded.ResultRoot, runID)
	if err := assertSafeRemoteRunDir(dir, runID); err != nil {
		return fmt.Errorf("local results: %w", err)
	}
	if _, err := os.Stat(dir); os.IsNotExist(err) {
		progress.Printf("cleanup: local results absent (%s)", dir)
		return nil
	}
	progress.Printf("cleanup: remove local results %s", dir)
	return os.RemoveAll(dir)
}

func (o *Orchestrator) removeLocalRunState(runID string) error {
	dir := o.StateStore.RunDir(runID)
	if err := assertSafeRemoteRunDir(dir, runID); err != nil {
		return fmt.Errorf("local state: %w", err)
	}
	progress.Printf("cleanup: remove local run state %s", dir)
	return os.RemoveAll(dir)
}

func (o *Orchestrator) cleanupLocalDeployManifest() error {
	if !usesLocalRuntime(o.Profile) {
		return nil
	}
	root, err := paths.ExpandHome(o.Expanded.RemoteRoot)
	if err != nil {
		return err
	}
	if abs, err := filepath.Abs(root); err == nil {
		root = abs
	}
	manifestPath := deploy.DeployManifestPath(root)
	if _, err := os.Stat(manifestPath); err != nil {
		if os.IsNotExist(err) {
			progress.Printf("cleanup: no local deploy manifest under %s", root)
			return nil
		}
		return err
	}
	progress.Printf("cleanup: removing local deploy manifest paths under %s", root)
	return deploy.Cleanup(root, true)
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

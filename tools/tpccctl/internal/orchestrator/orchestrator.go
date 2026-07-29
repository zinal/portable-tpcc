package orchestrator

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"portable-tpcc/tools/tpccctl/internal/config"
	"portable-tpcc/tools/tpccctl/internal/consolidate"
	"portable-tpcc/tools/tpccctl/internal/deploy"
	"portable-tpcc/tools/tpccctl/internal/profile"
	"portable-tpcc/tools/tpccctl/internal/redact"
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

	rc, err := config.BuildRunConfig(config.BuildInput{
		Profile:      o.Profile,
		ProfilePath:  o.Opts.ProfilePath,
		RunID:        runID,
		WorkerBinary: o.Opts.WorkerBinary,
	})
	if err != nil {
		return nil, err
	}

	runDir := o.StateStore.RunDir(runID)
	if err := os.MkdirAll(runDir, 0755); err != nil {
		return nil, err
	}
	if err := state.WriteJSON(runDir, "run-config.json", rc); err != nil {
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

// Deploy distributes local artifacts to target hosts (local mode).
func (o *Orchestrator) Deploy(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StateDeploying); err != nil {
		return err
	}
	ld := &deploy.LocalDeploy{
		SourceRoot: o.Expanded.LocalArtifacts,
		TargetRoot: o.Expanded.RemoteRoot,
	}
	_, err := ld.Deploy(o.Expanded.LocalArtifacts, true)
	if err != nil {
		o.StateStore.Fail(ctx.RunID, err)
		return err
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
		{"collect", func() error { return o.collect(ctx) }},
		{"consolidate", func() error { return o.consolidate(ctx) }},
	}
	for _, step := range steps {
		if o.shouldSkip(step.name) {
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
	return o.StateStore.Transition(ctx.RunID, state.StateSchema)
}

// RunSchema transitions to schema state.
func (o *Orchestrator) RunSchema(ctx *Context) error {
	return o.schema(ctx)
}

func (o *Orchestrator) load(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StateLoading); err != nil {
		return err
	}
	return o.StateStore.Transition(ctx.RunID, state.StateCheckingImport)
}

// RunLoad transitions through loading states.
func (o *Orchestrator) RunLoad(ctx *Context) error {
	return o.load(ctx)
}

func (o *Orchestrator) check(ctx *Context, phase string) error {
	if phase == "after-import" {
		return o.StateStore.Transition(ctx.RunID, state.StateCheckingImport)
	}
	return o.StateStore.Transition(ctx.RunID, state.StateCheckingResult)
}

// RunCheck records check phase in run state.
func (o *Orchestrator) RunCheck(ctx *Context, phase string) error {
	return o.check(ctx, phase)
}

func (o *Orchestrator) start(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StatePreparing); err != nil {
		return err
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateArming); err != nil {
		return err
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateRamping); err != nil {
		return err
	}
	if err := o.StateStore.Transition(ctx.RunID, state.StateMeasuring); err != nil {
		return err
	}
	return o.StateStore.Transition(ctx.RunID, state.StateDraining)
}

// RunStart executes prepare/arm/ramp/measure/drain state transitions.
func (o *Orchestrator) RunStart(ctx *Context) error {
	return o.start(ctx)
}

func (o *Orchestrator) collect(ctx *Context) error {
	return o.StateStore.Transition(ctx.RunID, state.StateCollecting)
}

// RunCollect transitions to collecting state.
func (o *Orchestrator) RunCollect(ctx *Context) error {
	return o.collect(ctx)
}

func (o *Orchestrator) consolidate(ctx *Context) error {
	if err := o.StateStore.Transition(ctx.RunID, state.StateConsolidating); err != nil {
		return err
	}
	cons := &consolidate.Consolidator{ResultRoot: o.Expanded.ResultRoot}
	agg, err := cons.Consolidate(ctx.RunID, ctx.RunConfig)
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
	return o.StateStore.Load(runID)
}

// Cleanup deploy artifacts.
func (o *Orchestrator) Cleanup(yes bool) error {
	return deploy.Cleanup(o.Expanded.RemoteRoot, yes)
}

// WritePlanJSON encodes plan snapshot to JSON.
func WritePlanJSON(plan *config.PlanSnapshot) ([]byte, error) {
	return json.MarshalIndent(plan, "", "  ")
}

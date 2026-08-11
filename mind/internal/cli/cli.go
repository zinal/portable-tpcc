package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/orchestrator"
)

// Config holds global CLI flags.
type Config struct {
	ProfilePath  string
	RunID        string
	WorkerBinary string
	SkipSteps    []string
	Yes          bool
	CheckPhase   string
}

// Run dispatches mind-tpcc subcommands (specification §9).
func Run(args []string) int {
	if len(args) == 0 {
		printUsage()
		return 2
	}
	// Help must work without required flags such as --profile.
	if wantsHelp(args) {
		printUsage()
		return 0
	}
	cfg := &Config{}
	cmd := args[0]
	rest := args[1:]
	for i := 0; i < len(rest); i++ {
		switch rest[i] {
		case "--profile":
			if i+1 >= len(rest) {
				fmt.Fprintln(os.Stderr, "--profile requires a value")
				return 2
			}
			cfg.ProfilePath = rest[i+1]
			i++
		case "--run-id":
			if i+1 >= len(rest) {
				fmt.Fprintln(os.Stderr, "--run-id requires a value")
				return 2
			}
			cfg.RunID = rest[i+1]
			i++
		case "--worker-binary":
			if i+1 >= len(rest) {
				fmt.Fprintln(os.Stderr, "--worker-binary requires a value")
				return 2
			}
			cfg.WorkerBinary = rest[i+1]
			i++
		case "--skip":
			if i+1 >= len(rest) {
				fmt.Fprintln(os.Stderr, "--skip requires a value")
				return 2
			}
			cfg.SkipSteps = append(cfg.SkipSteps, rest[i+1])
			i++
		case "--yes":
			cfg.Yes = true
		case "--after-import":
			cfg.CheckPhase = "after-import"
		case "--after-run":
			cfg.CheckPhase = "after-run"
		}
	}

	if cfg.ProfilePath == "" {
		fmt.Fprintln(os.Stderr, "error: --profile is required")
		return 2
	}

	opts := orchestrator.Options{
		ProfilePath:  cfg.ProfilePath,
		RunID:        cfg.RunID,
		WorkerBinary: cfg.WorkerBinary,
		SkipSteps:    cfg.SkipSteps,
	}

	switch cmd {
	case "validate":
		return runValidate(opts)
	case "plan":
		return runPlan(opts)
	case "deploy":
		return runDeploy(opts)
	case "schema":
		return runStage(opts, "schema")
	case "load":
		return runStage(opts, "load")
	case "check":
		return runCheck(opts, cfg.CheckPhase)
	case "start":
		return runStage(opts, "start")
	case "status":
		return runStatus(opts)
	case "stop":
		return runStop(opts)
	case "collect":
		return runStage(opts, "collect")
	case "consolidate":
		return runStage(opts, "consolidate")
	case "run":
		return runFull(opts)
	case "cleanup":
		return runCleanup(opts, cfg.Yes)
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n", cmd)
		printUsage()
		return 2
	}
}

func wantsHelp(args []string) bool {
	if len(args) == 0 {
		return false
	}
	switch args[0] {
	case "help", "-h", "--help":
		return true
	}
	for _, a := range args[1:] {
		if a == "-h" || a == "--help" {
			return true
		}
	}
	return false
}

func orch(opts orchestrator.Options) (*orchestrator.Orchestrator, error) {
	return orchestrator.New(opts)
}

func runValidate(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	res := o.Validate()
	out, _ := json.MarshalIndent(res, "", "  ")
	fmt.Println(string(out))
	if !res.Valid {
		return 1
	}
	return 0
}

func runPlan(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	var plan *config.PlanSnapshot
	if err := withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error {
		plan = config.BuildPlanSnapshot(ctx.RunConfig)
		return nil
	}); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	data, err := orchestrator.WritePlanJSON(plan)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Println(string(data))
	return 0
}

func runDeploy(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error { return o.Deploy(ctx) }); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

func runStage(opts orchestrator.Options, stage string) int {
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	var runErr error
	runErr = withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error {
		switch stage {
		case "schema":
			return o.RunSchema(ctx)
		case "load":
			return o.RunLoad(ctx)
		case "start":
			return o.RunStart(ctx)
		case "collect":
			return o.RunCollect(ctx)
		case "consolidate":
			return o.RunConsolidate(ctx)
		default:
			return fmt.Errorf("unknown stage %s", stage)
		}
	})
	if runErr != nil {
		fmt.Fprintln(os.Stderr, runErr)
		return 1
	}
	return 0
}

func runCheck(opts orchestrator.Options, phase string) int {
	if phase == "" {
		fmt.Fprintln(os.Stderr, "check requires --after-import or --after-run")
		return 2
	}
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error { return o.RunCheck(ctx, phase) }); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

func runStatus(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	runID := opts.RunID
	if runID == "" {
		runID = "latest"
	}
	rs, err := o.Status(runID)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	data, _ := json.MarshalIndent(rs, "", "  ")
	fmt.Println(string(data))
	return 0
}

func runStop(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error { return o.Stop(ctx) }); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

func runFull(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := o.Run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

func withMaterializedProfileLock(o *orchestrator.Orchestrator, fn func(*orchestrator.Context) error) error {
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
	ctx, err := o.Materialize()
	if err != nil {
		return err
	}
	return fn(ctx)
}

func runCleanup(opts orchestrator.Options, yes bool) int {
	if !yes {
		fmt.Fprintln(os.Stderr, "cleanup requires --yes")
		return 2
	}
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := withExistingRunCleanup(o, func(ctx *orchestrator.Context) error {
		return o.Cleanup(ctx, true)
	}); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

// withExistingRunCleanup locks the profile and loads an existing run (no new run_id).
func withExistingRunCleanup(o *orchestrator.Orchestrator, fn func(*orchestrator.Context) error) error {
	runID, err := o.ResolveCleanupRunID()
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
	ctx, err := o.LoadExistingContext(runID)
	if err != nil {
		return err
	}
	return fn(ctx)
}

func printUsage() {
	usage := strings.TrimSpace(`
mind-tpcc — portable-tpcc orchestrator

Usage:
  mind-tpcc <command> --profile <path> [options]

Commands:
  validate    Validate profile without side effects
  plan        Show planned assignment and argv
  deploy      Deploy binaries and schema to runtime hosts
  schema      Apply database schema
  load        Run horizontal data load
  check       Run checks (--after-import or --after-run)
  start       Arm workers and run measurement phases
  status      Show run state
  stop        Stop workers gracefully
  collect     Collect artifacts from runtime hosts
  consolidate Merge worker results into aggregate.json
  run         Full pipeline
  cleanup     Full teardown for a run: stop, DB clean, remote+local artifacts (--yes)

Options:
  --profile <path>         Profile YAML path
  --run-id <id>            Run identifier (default: continue latest active run
                           for this profile, else allocate a new id)
  --worker-binary <path>   Worker binary path
  --skip <step>            Skip pipeline step
  --yes                    Non-interactive confirmation
`)
	fmt.Println(usage)
}

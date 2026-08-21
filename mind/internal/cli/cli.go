package cli

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/orchestrator"
)

// Config holds global CLI flags.
type Config struct {
	ProfilePath    string
	RunID          string
	WorkerBinary   string
	SkipSteps      []string
	Overrides      config.ProfileOverrides
	Yes            bool
	CheckPhase     string
	LeaveProcesses bool
	CheckThreads   *int
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

	// Catch Ctrl+C / SIGTERM so deferred profile-lock release can run.
	interrupt, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	return run(args, interrupt)
}

// run is the testable entrypoint; interrupt is cancelled on SIGINT/SIGTERM in Run.
func run(args []string, interrupt context.Context) int {
	cfg := &Config{}
	cmd := args[0]
	rest := args[1:]
	for i := 0; i < len(rest); i++ {
		arg := rest[i]
		switch {
		case arg == "--profile" || strings.HasPrefix(arg, "--profile="):
			val, next, err := requireFlagValue(rest, i, "--profile")
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 2
			}
			cfg.ProfilePath = val
			i = next
		case arg == "--run-id" || strings.HasPrefix(arg, "--run-id="):
			val, next, err := requireFlagValue(rest, i, "--run-id")
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 2
			}
			cfg.RunID = val
			i = next
		case arg == "--worker-binary" || strings.HasPrefix(arg, "--worker-binary="):
			val, next, err := requireFlagValue(rest, i, "--worker-binary")
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 2
			}
			cfg.WorkerBinary = val
			i = next
		case arg == "--skip" || strings.HasPrefix(arg, "--skip="):
			val, next, err := requireFlagValue(rest, i, "--skip")
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 2
			}
			cfg.SkipSteps = append(cfg.SkipSteps, val)
			i = next
		case arg == "--warehouses" || strings.HasPrefix(arg, "--warehouses="):
			w, next, err := requireFlagInt(rest, i, "--warehouses")
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 2
			}
			cfg.Overrides.Warehouses = &w
			i = next
		case arg == "--ramp-up" || strings.HasPrefix(arg, "--ramp-up="):
			val, next, err := requireFlagValue(rest, i, "--ramp-up")
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 2
			}
			cfg.Overrides.RampUp = &val
			i = next
		case arg == "--measurement" || strings.HasPrefix(arg, "--measurement="):
			val, next, err := requireFlagValue(rest, i, "--measurement")
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 2
			}
			cfg.Overrides.Measurement = &val
			i = next
		case arg == "--threads" || strings.HasPrefix(arg, "--threads="):
			n, next, err := requireFlagNonNegativeInt(rest, i, "--threads")
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 2
			}
			cfg.CheckThreads = &n
			i = next
		case arg == "--yes":
			cfg.Yes = true
		case arg == "--leave-processes":
			cfg.LeaveProcesses = true
		case arg == "--after-import":
			cfg.CheckPhase = "after-import"
		case arg == "--after-test", arg == "--after-run":
			cfg.CheckPhase = "after-test"
		default:
			if strings.HasPrefix(arg, "-") {
				fmt.Fprintf(os.Stderr, "error: unknown flag %s\n", arg)
				return 2
			}
			fmt.Fprintf(os.Stderr, "error: unexpected argument %q\n", arg)
			return 2
		}
	}

	if cfg.ProfilePath == "" {
		fmt.Fprintln(os.Stderr, "error: --profile is required")
		return 2
	}

	opts := orchestrator.Options{
		ProfilePath:    cfg.ProfilePath,
		RunID:          cfg.RunID,
		WorkerBinary:   cfg.WorkerBinary,
		SkipSteps:      cfg.SkipSteps,
		Overrides:      cfg.Overrides,
		Interrupt:      interrupt,
		LeaveProcesses: cfg.LeaveProcesses,
		CheckThreads:   cfg.CheckThreads,
	}

	switch cmd {
	case "validate":
		return runValidate(opts)
	case "plan":
		return runPlan(opts)
	case "deploy":
		return runDeploy(opts)
	case "undeploy":
		return runUndeploy(opts, cfg.Yes)
	case "schema":
		return runStage(opts, "schema")
	case "load":
		return runStage(opts, "load")
	case "indexes":
		return runStage(opts, "indexes")
	case "check":
		return runCheck(opts, cfg.CheckPhase)
	case "test", "start":
		return runStage(opts, "test")
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

func exitErr(err error) int {
	fmt.Fprintln(os.Stderr, err)
	if errors.Is(err, orchestrator.ErrInterrupted) {
		return 130
	}
	return 1
}

func runValidate(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		return exitErr(err)
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
		return exitErr(err)
	}
	var plan *config.PlanSnapshot
	if err := withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error {
		plan = config.BuildPlanSnapshot(ctx.RunConfig, o.Opts.CheckThreads)
		return nil
	}); err != nil {
		return exitErr(err)
	}
	data, err := orchestrator.WritePlanJSON(plan)
	if err != nil {
		return exitErr(err)
	}
	fmt.Println(string(data))
	return 0
}

func runDeploy(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		return exitErr(err)
	}
	// Profile-scoped: no run_id allocation, materialize, or FSM transitions.
	if err := o.Deploy(); err != nil {
		return exitErr(err)
	}
	return 0
}

func runUndeploy(opts orchestrator.Options, yes bool) int {
	if !yes {
		fmt.Fprintln(os.Stderr, "undeploy requires --yes")
		return 2
	}
	o, err := orch(opts)
	if err != nil {
		return exitErr(err)
	}
	// Profile-scoped: inverse of deploy; no run_id / FSM.
	if err := o.Undeploy(true); err != nil {
		return exitErr(err)
	}
	return 0
}

func runStage(opts orchestrator.Options, stage string) int {
	o, err := orch(opts)
	if err != nil {
		return exitErr(err)
	}
	runErr := withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error {
		switch stage {
		case "schema":
			return o.RunSchema(ctx)
		case "load":
			return o.RunLoad(ctx)
		case "indexes":
			return o.RunIndexes(ctx)
		case "test":
			return o.RunTest(ctx)
		case "collect":
			return o.RunCollect(ctx)
		case "consolidate":
			return o.RunConsolidate(ctx)
		default:
			return fmt.Errorf("unknown stage %s", stage)
		}
	})
	if runErr != nil {
		return exitErr(runErr)
	}
	return 0
}

func runCheck(opts orchestrator.Options, phase string) int {
	if phase == "" {
		fmt.Fprintln(os.Stderr, "check requires --after-import or --after-test")
		return 2
	}
	o, err := orch(opts)
	if err != nil {
		return exitErr(err)
	}
	if err := withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error { return o.RunCheck(ctx, phase) }); err != nil {
		return exitErr(err)
	}
	return 0
}

func runStatus(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		return exitErr(err)
	}
	runID := opts.RunID
	if runID == "" {
		runID = "latest"
	}
	rs, err := o.Status(runID)
	if err != nil {
		return exitErr(err)
	}
	data, _ := json.MarshalIndent(rs, "", "  ")
	fmt.Println(string(data))
	return 0
}

func runStop(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		return exitErr(err)
	}
	if err := withMaterializedProfileLock(o, func(ctx *orchestrator.Context) error { return o.Stop(ctx) }); err != nil {
		return exitErr(err)
	}
	return 0
}

func runFull(opts orchestrator.Options) int {
	o, err := orch(opts)
	if err != nil {
		return exitErr(err)
	}
	if err := o.Run(); err != nil {
		return exitErr(err)
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
		return exitErr(err)
	}
	if err := withExistingRunCleanup(o, func(ctx *orchestrator.Context) error {
		return o.Cleanup(ctx, true)
	}); err != nil {
		return exitErr(err)
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

func requireFlagValue(rest []string, i int, name string) (string, int, error) {
	arg := rest[i]
	if prefix := name + "="; strings.HasPrefix(arg, prefix) {
		return arg[len(prefix):], i, nil
	}
	if i+1 >= len(rest) {
		return "", i, fmt.Errorf("%s requires a value", name)
	}
	return rest[i+1], i + 1, nil
}

func requireFlagInt(rest []string, i int, name string) (int, int, error) {
	raw, next, err := requireFlagValue(rest, i, name)
	if err != nil {
		return 0, i, err
	}
	n, err := strconv.Atoi(raw)
	if err != nil {
		return 0, i, fmt.Errorf("%s: invalid integer %q", name, raw)
	}
	return n, next, nil
}

func requireFlagNonNegativeInt(rest []string, i int, name string) (int, int, error) {
	n, next, err := requireFlagInt(rest, i, name)
	if err != nil {
		return 0, i, err
	}
	if n < 0 {
		return 0, i, fmt.Errorf("%s must not be negative", name)
	}
	return n, next, nil
}

func printUsage() {
	usage := strings.TrimSpace(`
mind-tpcc — portable-tpcc orchestrator

Usage:
  mind-tpcc <command> --profile <path> [options]

Commands:
  validate    Validate profile without side effects
  plan        Show planned assignment and argv
  deploy      Deploy shared worker binary (profile-scoped; no run_id / FSM)
  undeploy    Remove shared worker binary (profile-scoped; --yes)
  schema      Apply database schema
  load        Run horizontal data load
  indexes     Create secondary indexes and gather statistics
  check       Run checks (--after-import or --after-test)
  test        Arm workers and run ramp-up / measurement / drain
  start       Alias for test
  status      Show run state
  stop        Stop workers gracefully
  collect     Collect artifacts from runtime hosts
  consolidate Merge worker results into aggregate.json and print brief stats
  run         Full pipeline (requires prior explicit deploy)
  cleanup     Full teardown for a run: stop, DB clean, remote+local run artifacts (--yes)

Options:
  --profile <path>         Profile YAML path
  --run-id <id>            Run identifier (default: continue latest active run
                           for this profile, else allocate a new id)
  --worker-binary <path>   Worker binary path
  --warehouses <n>         Override scale.warehouses (must be <= profile value)
  --ramp-up <duration>     Override phases.ramp_up (warmup), e.g. 30s, 5m
  --measurement <duration> Override phases.measurement, e.g. 2m, 120m
  --threads <n>            Override check session concurrency (0 = auto)
  --skip <step>            Skip pipeline step
  --yes                    Non-interactive confirmation
  --leave-processes        Debug: do not kill remote processes this
                           invocation launched when mind-tpcc exits
  --after-import           check: post-import integrity phase
  --after-test             check: post-test integrity phase
  --after-run              deprecated alias for --after-test
`)
	fmt.Println(usage)
}

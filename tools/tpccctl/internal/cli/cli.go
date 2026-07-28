package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"portable-tpcc/tools/tpccctl/internal/orchestrator"
)

// Config holds global CLI flags.
type Config struct {
	ProfilePath    string
	RunID          string
	SpecBinary     string
	WorkerBinary   string
	SourceRevision string
	SkipSteps      []string
	Yes            bool
	CheckPhase     string
}

// Run dispatches tpccctl subcommands per specification §8.2.
func Run(args []string) int {
	if len(args) == 0 {
		printUsage()
		return 2
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
		case "--spec-binary":
			if i+1 >= len(rest) {
				fmt.Fprintln(os.Stderr, "--spec-binary requires a value")
				return 2
			}
			cfg.SpecBinary = rest[i+1]
			i++
		case "--worker-binary":
			if i+1 >= len(rest) {
				fmt.Fprintln(os.Stderr, "--worker-binary requires a value")
				return 2
			}
			cfg.WorkerBinary = rest[i+1]
			i++
		case "--source-revision":
			if i+1 >= len(rest) {
				fmt.Fprintln(os.Stderr, "--source-revision requires a value")
				return 2
			}
			cfg.SourceRevision = rest[i+1]
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

	if cfg.ProfilePath == "" && cmd != "help" {
		fmt.Fprintln(os.Stderr, "error: --profile is required")
		return 2
	}

	opts := orchestrator.Options{
		ProfilePath:    cfg.ProfilePath,
		RunID:          cfg.RunID,
		SpecBinary:     cfg.SpecBinary,
		WorkerBinary:   cfg.WorkerBinary,
		SourceRevision: cfg.SourceRevision,
		SkipSteps:      cfg.SkipSteps,
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
	case "help", "-h", "--help":
		printUsage()
		return 0
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n", cmd)
		printUsage()
		return 2
	}
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
	plan, err := o.Plan()
	if err != nil {
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
	ctx, err := o.Materialize()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := o.Deploy(ctx); err != nil {
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
	ctx, err := o.Materialize()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	var runErr error
	switch stage {
	case "schema":
		runErr = o.RunSchema(ctx)
	case "load":
		runErr = o.RunLoad(ctx)
	case "start":
		runErr = o.RunStart(ctx)
	case "collect":
		runErr = o.RunCollect(ctx)
	case "consolidate":
		runErr = o.RunConsolidate(ctx)
	default:
		runErr = fmt.Errorf("unknown stage %s", stage)
	}
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
	ctx, err := o.Materialize()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := o.RunCheck(ctx, phase); err != nil {
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
	ctx, err := o.Materialize()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := o.StateStore.Transition(ctx.RunID, "stopping"); err != nil {
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

func runCleanup(opts orchestrator.Options, yes bool) int {
	o, err := orch(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := o.Cleanup(yes); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

func printUsage() {
	usage := strings.TrimSpace(`
tpccctl — portable-tpcc orchestrator

Usage:
  tpccctl <command> --profile <path> [options]

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
  cleanup     Remove deploy manifest paths (--yes required)

Options:
  --profile <path>         Profile YAML path
  --run-id <id>            Run identifier
  --spec-binary <path>     tpcc-spec binary path
  --worker-binary <path>   Worker binary path
  --source-revision <rev>  Source git revision
  --skip <step>            Skip pipeline step (engineering)
  --yes                    Non-interactive confirmation
`)
	fmt.Println(usage)
}

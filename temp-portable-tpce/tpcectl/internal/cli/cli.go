package cli

import (
	"context"
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/cleanup"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/collect"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/deploy"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/lifecycle"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/load"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/plan"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/run"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/schema"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/state"
)

// Global flags shared by commands.
type Global struct {
	File     string
	Verbose  bool
	DryRun   bool
	RunID    string
	StateDir string
	OnlyRole string
}

// ParseGlobals registers global flags on fs.
func ParseGlobals(fs *flag.FlagSet, g *Global) {
	fs.StringVar(&g.File, "f", "", "Profile YAML path")
	fs.StringVar(&g.File, "file", "", "Profile YAML path")
	fs.BoolVar(&g.Verbose, "v", false, "Verbose output")
	fs.BoolVar(&g.Verbose, "verbose", false, "Verbose output")
	fs.BoolVar(&g.DryRun, "dry-run", false, "Print actions without side effects")
	fs.StringVar(&g.RunID, "run-id", "", "Override run_id")
	fs.StringVar(&g.StateDir, "state-dir", "", "Local state directory")
	fs.StringVar(&g.OnlyRole, "only-role", "", "Limit operations to role (bh|mee|dm|ce|loader)")
}

// ResolveProfile loads, resolves, and validates a profile.
func ResolveProfile(g Global) (*config.ResolvedProfile, error) {
	if g.File == "" {
		return nil, fmt.Errorf("profile path is required (-f)")
	}
	p, err := config.Load(g.File)
	if err != nil {
		return nil, err
	}
	r, err := config.Resolve(p, g.File, g.RunID)
	if err != nil {
		return nil, err
	}
	if err := config.Validate(r); err != nil {
		return nil, err
	}
	return r, nil
}

// StateStore returns the configured state store.
func StateStore(g Global) (*state.Store, error) {
	dir := g.StateDir
	if dir == "" {
		var err error
		dir, err = state.DefaultStateDir()
		if err != nil {
			return nil, err
		}
	}
	return state.NewStore(dir), nil
}

// RunValidate executes the validate command.
func RunValidate(args []string) int {
	fs := flag.NewFlagSet("validate", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "validate: %v\n", err)
		return 1
	}
	fmt.Printf("profile %q is valid (run_id=%s)\n", r.Name, r.EffectiveRunID)
	return 0
}

// RunPlan executes the plan command.
func RunPlan(args []string) int {
	fs := flag.NewFlagSet("plan", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	var forOp string
	var baseTimeEpoch int64
	var hasBaseTime bool
	fs.StringVar(&forOp, "for", "", "Plan target: deploy|schema|load|run|start|stop")
	fs.Func("base-time-epoch", "Override base_time_epoch (Unix seconds)", func(s string) error {
		var v int64
		if _, err := fmt.Sscanf(s, "%d", &v); err != nil {
			return err
		}
		baseTimeEpoch = v
		hasBaseTime = true
		return nil
	})
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "plan: %v\n", err)
		return 1
	}

	opts := plan.Options{
		For:     plan.For(forOp),
		Now:     time.Now().UTC(),
		Verbose: g.Verbose,
	}
	if hasBaseTime {
		opts.BaseTimeEpoch = &baseTimeEpoch
	}
	if err := plan.Write(os.Stdout, r, opts); err != nil {
		fmt.Fprintf(os.Stderr, "plan: %v\n", err)
		return 1
	}
	return 0
}

// RunDeploy executes the deploy command.
func RunDeploy(args []string) int {
	fs := flag.NewFlagSet("deploy", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "deploy: %v\n", err)
		return 1
	}
	if err := deploy.Run(r, deploy.Options{DryRun: g.DryRun, Verbose: g.Verbose}, nil); err != nil {
		fmt.Fprintf(os.Stderr, "deploy: %v\n", err)
		return 1
	}
	if !g.DryRun {
		fmt.Printf("deploy complete for profile %q on %d host(s)\n", r.Name, len(r.DeployHosts()))
	}
	return 0
}

// RunCleanup executes the cleanup command.
func RunCleanup(args []string) int {
	fs := flag.NewFlagSet("cleanup", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	var yes bool
	var deleteRuns bool
	fs.BoolVar(&yes, "yes", false, "Confirm cleanup without prompt")
	fs.BoolVar(&deleteRuns, "delete-runs", false, "Also delete runs/<run-id> on runtime hosts")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "cleanup: %v\n", err)
		return 1
	}
	if err := cleanup.Run(r, cleanup.Options{
		Yes:        yes,
		DryRun:     g.DryRun,
		Verbose:    g.Verbose,
		DeleteRuns: deleteRuns,
		RunID:      g.RunID,
	}, nil); err != nil {
		fmt.Fprintf(os.Stderr, "cleanup: %v\n", err)
		return 1
	}
	if !g.DryRun {
		fmt.Printf("cleanup complete for profile %q\n", r.Name)
	}
	return 0
}

func lifecycleController(g Global, r *config.ResolvedProfile) (*lifecycle.Controller, *state.Store, error) {
	store, err := StateStore(g)
	if err != nil {
		return nil, nil, err
	}
	return &lifecycle.Controller{Profile: r, Store: store}, store, nil
}

// RunStart executes the start command.
func RunStart(args []string) int {
	fs := flag.NewFlagSet("start", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	var baseTimeEpoch int64
	var hasBaseTime bool
	var skipDBPing bool
	fs.BoolVar(&skipDBPing, "skip-db-ping", false, "Skip database TCP reachability check")
	fs.Func("base-time-epoch", "Override base_time_epoch (Unix seconds)", func(s string) error {
		var v int64
		if _, err := fmt.Sscanf(s, "%d", &v); err != nil {
			return err
		}
		baseTimeEpoch = v
		hasBaseTime = true
		return nil
	})
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "start: %v\n", err)
		return 1
	}
	ctrl, _, err := lifecycleController(g, r)
	if err != nil {
		fmt.Fprintf(os.Stderr, "start: %v\n", err)
		return 1
	}
	opts := lifecycle.Options{DryRun: g.DryRun, Verbose: g.Verbose, OnlyRole: g.OnlyRole, SkipDBPing: skipDBPing}
	if hasBaseTime {
		opts.BaseTimeEpoch = &baseTimeEpoch
	}
	if err := ctrl.Start(context.Background(), opts); err != nil {
		fmt.Fprintf(os.Stderr, "start: %v\n", err)
		return 1
	}
	if !g.DryRun {
		fmt.Printf("start complete: run_id=%s state=running\n", r.EffectiveRunID)
	}
	return 0
}

// RunStop executes the stop command.
func RunStop(args []string) int {
	fs := flag.NewFlagSet("stop", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "stop: %v\n", err)
		return 1
	}
	ctrl, _, err := lifecycleController(g, r)
	if err != nil {
		fmt.Fprintf(os.Stderr, "stop: %v\n", err)
		return 1
	}
	if err := ctrl.Stop(context.Background(), lifecycle.Options{DryRun: g.DryRun, Verbose: g.Verbose, OnlyRole: g.OnlyRole}, g.RunID); err != nil {
		fmt.Fprintf(os.Stderr, "stop: %v\n", err)
		return 1
	}
	if !g.DryRun {
		fmt.Printf("stop complete for profile %q\n", r.Name)
	}
	return 0
}

// RunStatus executes the status command.
func RunStatus(args []string) int {
	fs := flag.NewFlagSet("status", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "status: %v\n", err)
		return 1
	}
	ctrl, _, err := lifecycleController(g, r)
	if err != nil {
		fmt.Fprintf(os.Stderr, "status: %v\n", err)
		return 1
	}
	lines, err := ctrl.Status(g.RunID)
	if err != nil {
		fmt.Fprintf(os.Stderr, "status: %v\n", err)
		return 1
	}
	for _, line := range lines {
		fmt.Printf("%-10s %-10s host=%-8s pid=%-6d alive=%-5v listen=%-5v %s\n",
			line.Role, line.Name, line.Host, line.PID, line.Alive, line.Listening, line.Listen)
	}
	return 0
}

// RunSchema executes the schema command.
func RunSchema(args []string) int {
	fs := flag.NewFlagSet("schema", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	var recreate bool
	fs.BoolVar(&recreate, "recreate", false, "Drop and recreate public schema before applying DDL")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "schema: %v\n", err)
		return 1
	}
	opts := schema.Options{Recreate: recreate, DryRun: g.DryRun, Verbose: g.Verbose}
	if err := schema.Run(context.Background(), r, opts); err != nil {
		fmt.Fprintf(os.Stderr, "schema: %v\n", err)
		return 1
	}
	if !g.DryRun {
		fmt.Printf("schema complete for profile %q\n", r.Name)
	}
	return 0
}

// RunLoad executes the load command.
func RunLoad(args []string) int {
	fs := flag.NewFlagSet("load", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load: %v\n", err)
		return 1
	}
	opts := load.Options{DryRun: g.DryRun, Verbose: g.Verbose, OnlyRole: g.OnlyRole, RunID: g.RunID}
	if err := load.Run(context.Background(), r, opts, nil); err != nil {
		fmt.Fprintf(os.Stderr, "load: %v\n", err)
		return 1
	}
	if !g.DryRun {
		fmt.Printf("load complete for profile %q (%d shard(s))\n", r.Name, len(r.Load.Shards))
	}
	return 0
}

// RunCollect executes the collect command.
func RunCollect(args []string) int {
	fs := flag.NewFlagSet("collect", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "collect: %v\n", err)
		return 1
	}
	store, err := StateStore(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "collect: %v\n", err)
		return 1
	}
	opts := collect.Options{DryRun: g.DryRun, Verbose: g.Verbose, RunID: g.RunID}
	if err := collect.Run(context.Background(), r, store, opts, nil); err != nil {
		fmt.Fprintf(os.Stderr, "collect: %v\n", err)
		return 1
	}
	if !g.DryRun {
		fmt.Printf("collect complete into %s\n", r.Collect.Dest)
	}
	return 0
}

type runFlags struct {
	skipDeploy  bool
	skipSchema  bool
	skipLoad    bool
	skipCollect bool
	noStop      bool
	baseTime    *int64
}

func parseRunFlags(fs *flag.FlagSet) *runFlags {
	var rf runFlags
	fs.BoolVar(&rf.skipDeploy, "skip-deploy", false, "Skip deploy step")
	fs.BoolVar(&rf.skipSchema, "skip-schema", false, "Skip schema step")
	fs.BoolVar(&rf.skipLoad, "skip-load", false, "Skip load step")
	fs.BoolVar(&rf.skipCollect, "skip-collect", false, "Skip collect step")
	fs.BoolVar(&rf.noStop, "no-stop", false, "Leave processes running after measurement")
	fs.Func("base-time-epoch", "Override base_time_epoch (Unix seconds)", func(s string) error {
		var v int64
		if _, err := fmt.Sscanf(s, "%d", &v); err != nil {
			return err
		}
		rf.baseTime = &v
		return nil
	})
	return &rf
}

func runOptions(g Global, rf *runFlags) run.Options {
	return run.Options{
		SkipDeploy:    rf.skipDeploy,
		SkipSchema:    rf.skipSchema,
		SkipLoad:      rf.skipLoad,
		SkipCollect:   rf.skipCollect,
		NoStop:        rf.noStop,
		DryRun:        g.DryRun,
		Verbose:       g.Verbose,
		OnlyRole:      g.OnlyRole,
		RunID:         g.RunID,
		BaseTimeEpoch: rf.baseTime,
	}
}

// RunRun executes the run command.
func RunRun(args []string) int {
	fs := flag.NewFlagSet("run", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	rf := parseRunFlags(fs)
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "run: %v\n", err)
		return 1
	}
	store, err := StateStore(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "run: %v\n", err)
		return 1
	}
	ctrl := &run.Controller{Profile: r, Store: store}
	if err := ctrl.Run(context.Background(), runOptions(g, rf)); err != nil {
		fmt.Fprintf(os.Stderr, "run: %v\n", err)
		return 1
	}
	if !g.DryRun {
		fmt.Printf("run complete for profile %q (run_id=%s)\n", r.Name, r.EffectiveRunID)
	}
	return 0
}

// RunSmoke executes a short run with duration/users overrides.
func RunSmoke(args []string) int {
	fs := flag.NewFlagSet("smoke", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var g Global
	ParseGlobals(fs, &g)
	rf := parseRunFlags(fs)
	var durationSec int = 120
	var users int
	var hasUsers bool
	fs.IntVar(&durationSec, "duration-sec", 120, "Measurement duration override (seconds)")
	fs.Func("users", "Override CE/standalone users", func(s string) error {
		var v int
		if _, err := fmt.Sscanf(s, "%d", &v); err != nil {
			return err
		}
		users = v
		hasUsers = true
		return nil
	})
	if err := fs.Parse(args); err != nil {
		return 2
	}
	r, err := ResolveProfile(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "smoke: %v\n", err)
		return 1
	}
	overrides := config.RunOverrides{DurationSec: &durationSec}
	if hasUsers {
		overrides.Users = &users
	}
	r = config.ApplyRunOverrides(r, overrides)

	store, err := StateStore(g)
	if err != nil {
		fmt.Fprintf(os.Stderr, "smoke: %v\n", err)
		return 1
	}
	ctrl := &run.Controller{Profile: r, Store: store}
	if err := ctrl.Run(context.Background(), runOptions(g, rf)); err != nil {
		fmt.Fprintf(os.Stderr, "smoke: %v\n", err)
		return 1
	}
	if !g.DryRun {
		fmt.Printf("smoke complete for profile %q (run_id=%s, duration_sec=%d)\n",
			r.Name, r.EffectiveRunID, durationSec)
	}
	return 0
}

// RunNotImplemented is a placeholder for commands beyond M5.
func RunNotImplemented(name string, args []string) int {
	fmt.Fprintf(os.Stderr, "%s: not implemented yet (M2+)\n", name)
	return 1
}

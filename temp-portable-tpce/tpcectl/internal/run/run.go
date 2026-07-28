package run

import (
	"context"
	"fmt"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/collect"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/deploy"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/lifecycle"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/load"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/schema"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/state"
)

// Options controls the full run lifecycle (spec-orchestrator §4.3, §6.3).
type Options struct {
	SkipDeploy    bool
	SkipSchema    bool
	SkipLoad      bool
	SkipCollect   bool
	NoStop        bool
	DryRun        bool
	Verbose       bool
	OnlyRole      string
	RunID         string
	BaseTimeEpoch *int64
}

// Controller executes deploy → schema → load → start → wait → stop → collect.
type Controller struct {
	Profile *config.ResolvedProfile
	Store   *state.Store
}

// Run performs the complete benchmark lifecycle.
func (c *Controller) Run(ctx context.Context, opts Options) error {
	if c.Profile == nil || c.Store == nil {
		return fmt.Errorf("controller is not configured")
	}
	profile := c.Profile

	if opts.DryRun {
		fmt.Println("dry-run: would execute full run sequence")
		printDryRunSteps(profile, opts)
		return nil
	}

	lc := &lifecycle.Controller{Profile: profile, Store: c.Store}

	if !opts.SkipDeploy {
		if opts.Verbose {
			fmt.Println("run: deploy")
		}
		if err := deploy.Run(profile, deploy.Options{Verbose: opts.Verbose}, nil); err != nil {
			return fmt.Errorf("deploy: %w", err)
		}
	}

	if !opts.SkipSchema {
		if opts.Verbose {
			fmt.Println("run: schema")
		}
		if err := schema.Run(ctx, profile, schema.Options{Verbose: opts.Verbose}); err != nil {
			return fmt.Errorf("schema: %w", err)
		}
	}

	if !opts.SkipLoad && len(profile.Load.Shards) > 0 {
		if opts.Verbose {
			fmt.Println("run: load")
		}
		if err := load.Run(ctx, profile, load.Options{
			Verbose:  opts.Verbose,
			OnlyRole: opts.OnlyRole,
			RunID:    opts.RunID,
		}, nil); err != nil {
			return fmt.Errorf("load: %w", err)
		}
	}

	if opts.Verbose {
		fmt.Println("run: start")
	}
	startOpts := lifecycle.Options{
		Verbose:    opts.Verbose,
		OnlyRole:   opts.OnlyRole,
		BaseTimeEpoch: opts.BaseTimeEpoch,
	}
	if err := lc.Start(ctx, startOpts); err != nil {
		return fmt.Errorf("start: %w", err)
	}

	runFailed := false
	waitErr := lc.WaitCompletion(ctx, opts.RunID, opts.Verbose)
	if waitErr != nil {
		runFailed = true
		if opts.Verbose {
			fmt.Printf("run: wait completion: %v\n", waitErr)
		}
	}

	if !opts.NoStop {
		if opts.Verbose {
			fmt.Println("run: stop")
		}
		if err := lc.Stop(ctx, lifecycle.Options{Verbose: opts.Verbose, OnlyRole: opts.OnlyRole}, opts.RunID); err != nil {
			if waitErr == nil {
				return fmt.Errorf("stop: %w", err)
			}
		}
	}

	if !opts.SkipCollect {
		if opts.Verbose {
			fmt.Println("run: collect")
		}
		if err := collect.Run(ctx, profile, c.Store, collect.Options{
			Verbose: opts.Verbose,
			RunID:   opts.RunID,
		}, nil); err != nil {
			return fmt.Errorf("collect: %w", err)
		}
	}

	if runFailed {
		return waitErr
	}
	return nil
}

func printDryRunSteps(profile *config.ResolvedProfile, opts Options) {
	if !opts.SkipDeploy {
		fmt.Println("  deploy")
	}
	if !opts.SkipSchema {
		fmt.Println("  schema")
	}
	if !opts.SkipLoad && len(profile.Load.Shards) > 0 {
		fmt.Println("  load")
	}
	fmt.Println("  start")
	fmt.Println("  wait for CE/standalone completion")
	if !opts.NoStop {
		fmt.Println("  stop")
	}
	if !opts.SkipCollect {
		fmt.Println("  collect")
	}
}

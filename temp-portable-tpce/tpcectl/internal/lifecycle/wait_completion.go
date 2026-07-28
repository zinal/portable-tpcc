package lifecycle

import (
	"context"
	"fmt"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/process"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/state"
)

// WaitCompletion blocks until CE or standalone Driver processes exit (§4.3).
func (c *Controller) WaitCompletion(ctx context.Context, runID string, verbose bool) error {
	if c.Profile == nil || c.Store == nil {
		return fmt.Errorf("controller is not configured")
	}
	if c.Dial == nil {
		c.Dial = remote.DefaultExecutorDialer()
	}

	profileID := state.ProfileID(c.Profile)
	resolvedRunID, err := c.Store.ResolveRunID(profileID, runID)
	if err != nil {
		return err
	}
	runState, err := c.Store.LoadRunState(resolvedRunID)
	if err != nil {
		return err
	}

	targets := measurementProcesses(runState)
	if len(targets) == 0 {
		return fmt.Errorf("no CE or standalone processes in run-state")
	}

	t0 := runState.MeasurementStartedAt
	if t0.IsZero() {
		t0 = runState.StartedAt
	}
	if t0.IsZero() {
		t0 = time.Now().UTC()
	}

	duration := time.Duration(c.Profile.EffectiveDurationSec()) * time.Second
	minRun := t0.Add(duration)
	deadline := t0.Add(duration + c.Profile.Timeouts.CECompletionGrace)

	executors := newExecutorCache(c.Dial, c.Profile)
	defer executors.closeAll()

	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()

	for {
		allDone := true
		for _, p := range targets {
			ex := executors.get(p.Host)
			if ex == nil {
				return fmt.Errorf("connect to host %s", p.Host)
			}
			alive, err := process.IsAlive(ex, p.PID)
			if err != nil {
				return fmt.Errorf("check %s %s: %w", p.Role, p.Name, err)
			}
			if alive {
				allDone = false
				continue
			}
			if time.Now().Before(minRun) {
				return fmt.Errorf("%s %s exited before effective duration_sec elapsed", p.Role, p.Name)
			}
		}
		if allDone {
			if verbose {
				fmt.Printf("measurement complete for run_id=%s\n", resolvedRunID)
			}
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("timed out waiting for CE/standalone completion (deadline %s)", deadline.UTC().Format(time.RFC3339))
		}

		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
		}
	}
}

func measurementProcesses(runState *state.RunState) []state.ProcessRecord {
	standalone := runState.ProcessesByRole("standalone")
	if len(standalone) > 0 {
		return standalone
	}
	return runState.ProcessesByRole("ce")
}

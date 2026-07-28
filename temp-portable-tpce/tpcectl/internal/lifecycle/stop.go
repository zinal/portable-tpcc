package lifecycle

import (
	"context"
	"fmt"
	"net"
	"sort"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/argv"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/process"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/state"
)

// Stop runs the normative shutdown sequence (§9.6).
func (c *Controller) Stop(ctx context.Context, opts Options, runID string) error {
	if c.Profile == nil || c.Store == nil {
		return fmt.Errorf("controller is not configured")
	}
	if c.Dial == nil {
		c.Dial = remote.DefaultExecutorDialer()
	}
	if opts.DryRun {
		fmt.Println("dry-run: would execute stop sequence")
		return nil
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

	runState.MarkStopping()
	if err := c.Store.SaveRunState(runState); err != nil {
		return err
	}

	executors := newExecutorCache(c.Dial, c.Profile)
	defer executors.closeAll()

	standalone := runState.ProcessesByRole("standalone")
	ce := runState.ProcessesByRole("ce")
	dm := runState.ProcessesByRole("dm")
	mee := runState.ProcessesByRole("mee")
	bh := runState.ProcessesByRole("bh")

	if opts.OnlyRole != "" {
		standalone = filterProcRole(standalone, opts.OnlyRole)
		ce = filterProcRole(ce, opts.OnlyRole)
		dm = filterProcRole(dm, opts.OnlyRole)
		mee = filterProcRole(mee, opts.OnlyRole)
		bh = filterProcRole(bh, opts.OnlyRole)
	}

	grace := c.Profile.Timeouts.StopGrace

	// Step 1: CE or standalone
	if len(standalone) > 0 {
		if err := stopProcesses(executors, standalone, grace); err != nil {
			return err
		}
	} else {
		if err := stopProcesses(executors, ce, grace); err != nil {
			return err
		}
	}

	// Step 2: drain
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-time.After(c.Profile.Timeouts.MEEDrain):
	}

	// Step 3: DM (skip if standalone already stopped)
	if len(standalone) == 0 {
		if err := stopProcesses(executors, dm, grace); err != nil {
			return err
		}
	}

	// Step 4: MEE
	if err := stopProcesses(executors, mee, grace); err != nil {
		return err
	}

	// Step 5: BH
	if err := stopProcesses(executors, bh, grace); err != nil {
		return err
	}

	runState.MarkCompleted()
	return c.Store.SaveRunState(runState)
}

func stopProcesses(executors *executorCache, procs []state.ProcessRecord, grace time.Duration) error {
	for _, p := range procs {
		ex := executors.get(p.Host)
		if ex == nil {
			return fmt.Errorf("connect to host %s", p.Host)
		}
		if err := process.Stop(ex, p.PID, grace); err != nil {
			return fmt.Errorf("stop %s %s (pid %d): %w", p.Role, p.Name, p.PID, err)
		}
	}
	return nil
}

func filterProcRole(procs []state.ProcessRecord, role string) []state.ProcessRecord {
	if role == "" {
		return procs
	}
	var out []state.ProcessRecord
	for _, p := range procs {
		if p.Role == role {
			out = append(out, p)
		}
	}
	return out
}

// StatusLine is one process status row.
type StatusLine struct {
	Role      string
	Name      string
	Host      string
	PID       int
	Alive     bool
	Listening bool
	Listen    string
}

// Status reports process and listen state from run-state.
func (c *Controller) Status(runID string) ([]StatusLine, error) {
	if c.Profile == nil || c.Store == nil {
		return nil, fmt.Errorf("controller is not configured")
	}
	if c.Dial == nil {
		c.Dial = remote.DefaultExecutorDialer()
	}

	profileID := state.ProfileID(c.Profile)
	resolvedRunID, err := c.Store.ResolveRunID(profileID, runID)
	if err != nil {
		return nil, err
	}
	runState, err := c.Store.LoadRunState(resolvedRunID)
	if err != nil {
		return nil, err
	}

	executors := newExecutorCache(c.Dial, c.Profile)
	defer executors.closeAll()

	var lines []StatusLine
	for _, p := range runState.Processes {
		ex := executors.get(p.Host)
		alive := false
		if ex != nil {
			alive, _ = process.IsAlive(ex, p.PID)
		}
		host, port, listenStr, listening := listenStatus(c.Profile, p)
		lines = append(lines, StatusLine{
			Role: p.Role, Name: p.Name, Host: p.Host, PID: p.PID,
			Alive: alive, Listening: listening, Listen: listenStr,
		})
		_ = host
		_ = port
	}
	sort.Slice(lines, func(i, j int) bool {
		if lines[i].Role == lines[j].Role {
			return lines[i].Name < lines[j].Name
		}
		return lines[i].Role < lines[j].Role
	})
	return lines, nil
}

func listenStatus(profile *config.ResolvedProfile, p state.ProcessRecord) (host string, port int, listenStr string, ok bool) {
	instances, err := argvBuildForStatus(profile, p)
	if err != nil {
		return "", 0, "", false
	}
	for _, inst := range instances {
		if inst.Name != p.Name || inst.Role != p.Role {
			continue
		}
		h, pt, err := listenEndpoint(profile, inst)
		if err != nil {
			return "", 0, "", false
		}
		conn, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", h, pt), 2*time.Second)
		if err == nil {
			_ = conn.Close()
			return h, pt, fmt.Sprintf("%s:%d", h, pt), true
		}
		return h, pt, fmt.Sprintf("%s:%d", h, pt), false
	}
	return "", 0, "", false
}

func argvBuildForStatus(profile *config.ResolvedProfile, p state.ProcessRecord) ([]argv.InstanceArgv, error) {
	all, err := argv.BuildAll(profile)
	if err != nil {
		return nil, err
	}
	return all, nil
}

package lifecycle

import (
	"context"
	"fmt"
	"net"
	"sync"
	"time"

	"golang.org/x/sync/errgroup"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/argv"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/process"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/runconfigdist"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/runtimeconfig"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/sshx"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/state"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/wait"
)

// Options controls start/stop/status operations.
type Options struct {
	DryRun        bool
	Verbose       bool
	OnlyRole      string
	BaseTimeEpoch *int64
	SkipDBPing    bool
}

// Controller orchestrates process lifecycle (spec-orchestrator §4.3, §9).
type Controller struct {
	Profile *config.ResolvedProfile
	Store   *state.Store
	Dial    remote.ExecutorDialer
}

// Start runs the normative startup sequence through CE launch (§4.3 steps 1–8).
func (c *Controller) Start(ctx context.Context, opts Options) error {
	if c.Profile == nil || c.Store == nil {
		return fmt.Errorf("controller is not configured")
	}
	if c.Dial == nil {
		c.Dial = remote.DefaultExecutorDialer()
	}

	profile := c.Profile
	profileID := state.ProfileID(profile)

	if opts.DryRun {
		fmt.Println("dry-run: would execute start sequence")
		return nil
	}

	lock, err := c.Store.AcquireProfileLock(profileID)
	if err != nil {
		return err
	}
	defer lock.Release()

	active, _, err := c.Store.HasActiveRun(profileID)
	if err != nil {
		return err
	}
	if active {
		return fmt.Errorf("profile %q already has an active run", profile.Name)
	}

	if !opts.SkipDBPing {
		if err := pingDatabase(profile); err != nil {
			return err
		}
	}

	now := time.Now().UTC()
	_, raw, hash, err := runtimeconfig.Build(profile, runtimeconfig.BuildOptions{
		Now:           now,
		BaseTimeEpoch: opts.BaseTimeEpoch,
	})
	if err != nil {
		return err
	}
	doc, _, _, _ := runtimeconfig.Build(profile, runtimeconfig.BuildOptions{Now: now, BaseTimeEpoch: opts.BaseTimeEpoch})
	if err := config.ValidateBaseTimeEpochAtRun(doc.BaseTimeEpoch, now, profile.Timeouts); err != nil {
		return err
	}
	if len(profile.MEE) > 1 {
		if err := sshx.CheckMEEHostsClockSkew(profile); err != nil {
			return err
		}
	}

	runState, err := state.NewRunState(profile, hash, doc.BaseTimeEpoch)
	if err != nil {
		return err
	}
	runState.TouchStarted()
	if err := c.Store.SaveRunState(runState); err != nil {
		return err
	}
	if err := c.Store.SetCurrentRun(profileID, profile.EffectiveRunID); err != nil {
		return err
	}

	step2Start := time.Now()
	if err := runconfigdist.Distribute(ctx, profile, raw, hash, c.Dial, profile.Timeouts.ConfigDistribute); err != nil {
		runState.MarkFailed()
		_ = c.Store.SaveRunState(runState)
		return err
	}

	instances, err := argv.BuildAll(profile)
	if err != nil {
		return err
	}

	bh := instancesForStep(instances, "bh", opts.OnlyRole)
	mee := instancesForStep(instances, "mee", opts.OnlyRole)
	dm := instancesForStep(instances, "dm", opts.OnlyRole)
	ce := instancesForStep(instances, "ce", opts.OnlyRole)
	standalone := instancesForStep(instances, "standalone", opts.OnlyRole)

	executors := newExecutorCache(c.Dial, profile)
	defer executors.closeAll()

	// Step 3: BH
	if err := startRoleParallel(ctx, executors, profile, runState, c.Store, bh); err != nil {
		return c.fail(runState, err)
	}
	if err := waitListenRole(ctx, profile, bh, profile.Timeouts.Ready); err != nil {
		return c.fail(runState, err)
	}

	// Step 4: MEE
	if err := startRoleParallel(ctx, executors, profile, runState, c.Store, mee); err != nil {
		return c.fail(runState, err)
	}
	if err := waitListenRole(ctx, profile, mee, profile.Timeouts.Ready); err != nil {
		return c.fail(runState, err)
	}

	// Step 5: service-ready
	serviceDeadline := step2Start.Add(
		profile.Timeouts.ConfigDistribute +
			2*profile.Timeouts.Ready +
			time.Duration(profile.BaseTimeLeadSec)*time.Second +
			5*time.Second,
	)
	svcCtx, cancel := context.WithDeadline(ctx, serviceDeadline)
	defer cancel()
	if err := waitServiceReady(svcCtx, executors, append(bh, mee...)); err != nil {
		return c.fail(runState, err)
	}

	// Step 6–7: DM/standalone then CE
		if len(standalone) > 0 {
		if err := startSequential(ctx, executors, profile, runState, c.Store, standalone); err != nil {
			return c.fail(runState, err)
		}
		inst := standalone[0]
		ex := executors.get(inst.Host)
		stdoutRel := inst.Output + "/stdout.log"
		offset := process.StdoutSize(ex, stdoutRel)
		if err := process.WaitTradeCleanup(ex, stdoutRel, offset, profile.Timeouts.CleanupWait); err != nil {
			return c.fail(runState, err)
		}
		runState.MeasurementStartedAt = time.Now().UTC()
	} else {
		if err := startSequential(ctx, executors, profile, runState, c.Store, dm); err != nil {
			return c.fail(runState, err)
		}
		if len(dm) > 0 {
			inst := dm[0]
			ex := executors.get(inst.Host)
			stdoutRel := inst.Output + "/stdout.log"
			offset := process.StdoutSize(ex, stdoutRel)
			if err := process.WaitTradeCleanup(ex, stdoutRel, offset, profile.Timeouts.CleanupWait); err != nil {
				return c.fail(runState, err)
			}
		}
		if err := startRoleParallel(ctx, executors, profile, runState, c.Store, ce); err != nil {
			return c.fail(runState, err)
		}
		runState.MeasurementStartedAt = time.Now().UTC()
	}

	runState.MarkRunning()
	if err := c.Store.SaveRunState(runState); err != nil {
		return err
	}
	if opts.Verbose {
		fmt.Printf("run %s is running (%d processes)\n", profile.EffectiveRunID, len(runState.Processes))
	}
	return nil
}

func (c *Controller) fail(st *state.RunState, err error) error {
	st.MarkFailed()
	_ = c.Store.SaveRunState(st)
	return err
}

func pingDatabase(profile *config.ResolvedProfile) error {
	addr := fmt.Sprintf("%s:%d", profile.DB.Host, profile.DB.Port)
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return fmt.Errorf("database unreachable at %s: %w", addr, err)
	}
	_ = conn.Close()
	return nil
}

func filterRole(instances []argv.InstanceArgv, role string) []argv.InstanceArgv {
	var out []argv.InstanceArgv
	for _, i := range instances {
		if i.Role == role {
			out = append(out, i)
		}
	}
	return out
}

func instancesForStep(all []argv.InstanceArgv, role, onlyRole string) []argv.InstanceArgv {
	if onlyRole != "" && onlyRole != role {
		return nil
	}
	return filterRole(all, role)
}

type executorCache struct {
	dial    remote.ExecutorDialer
	profile *config.ResolvedProfile
	mu      sync.Mutex
	cache   map[string]remote.Executor
}

func newExecutorCache(dial remote.ExecutorDialer, profile *config.ResolvedProfile) *executorCache {
	return &executorCache{dial: dial, profile: profile, cache: make(map[string]remote.Executor)}
}

func (c *executorCache) get(host string) remote.Executor {
	c.mu.Lock()
	defer c.mu.Unlock()
	if ex, ok := c.cache[host]; ok {
		return ex
	}
	ex, err := c.dial(host, c.profile)
	if err != nil {
		return nil
	}
	c.cache[host] = ex
	return ex
}

func (c *executorCache) closeAll() {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, ex := range c.cache {
		_ = ex.Close()
	}
}

func startRoleParallel(
	ctx context.Context,
	executors *executorCache,
	profile *config.ResolvedProfile,
	runState *state.RunState,
	store *state.Store,
	instances []argv.InstanceArgv,
) error {
	if len(instances) == 0 {
		return nil
	}
	g, ctx := errgroup.WithContext(ctx)
	for _, inst := range instances {
		inst := inst
		g.Go(func() error {
			return startOne(ctx, executors, profile, runState, store, inst)
		})
	}
	return g.Wait()
}

func startSequential(
	ctx context.Context,
	executors *executorCache,
	profile *config.ResolvedProfile,
	runState *state.RunState,
	store *state.Store,
	instances []argv.InstanceArgv,
) error {
	for _, inst := range instances {
		if err := startOne(ctx, executors, profile, runState, store, inst); err != nil {
			return err
		}
	}
	return nil
}

func startOne(
	ctx context.Context,
	executors *executorCache,
	profile *config.ResolvedProfile,
	runState *state.RunState,
	store *state.Store,
	inst argv.InstanceArgv,
) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	ex := executors.get(inst.Host)
	if ex == nil {
		return fmt.Errorf("connect to host %s", inst.Host)
	}
	pid, pidFile, err := process.Start(ex, profile, profile.EffectiveRunID, inst)
	if err != nil {
		return err
	}
	rec := state.ProcessRecord{
		Role:      inst.Role,
		Name:      inst.Name,
		Host:      inst.Host,
		PID:       pid,
		PIDFile:   pidFile,
		Output:    remote.JoinRemoteAbs(profile.Paths.RemoteRoot, inst.Output),
		StartedAt: time.Now().UTC(),
	}
	runState.UpsertProcess(rec)
	return store.SaveRunState(runState)
}

func waitListenRole(ctx context.Context, profile *config.ResolvedProfile, instances []argv.InstanceArgv, timeout time.Duration) error {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	g, ctx := errgroup.WithContext(ctx)
	for _, inst := range instances {
		inst := inst
		g.Go(func() error {
			host, port, err := listenEndpoint(profile, inst)
			if err != nil {
				return err
			}
			return wait.TCPListen(ctx, host, port)
		})
	}
	return g.Wait()
}

func waitServiceReady(ctx context.Context, executors *executorCache, instances []argv.InstanceArgv) error {
	g, ctx := errgroup.WithContext(ctx)
	for _, inst := range instances {
		inst := inst
		g.Go(func() error {
			if inst.ReadyFile == "" {
				return fmt.Errorf("%s %s has no ready file", inst.Role, inst.Name)
			}
			ex := executors.get(inst.Host)
			if ex == nil {
				return fmt.Errorf("connect to host %s", inst.Host)
			}
			return wait.RemoteFile(ctx, ex, inst.ReadyFile)
		})
	}
	return g.Wait()
}

func listenEndpoint(profile *config.ResolvedProfile, inst argv.InstanceArgv) (string, int, error) {
	switch inst.Role {
	case "bh":
		for _, bh := range profile.BH {
			if bh.Name == inst.Name {
				addr, err := profile.HostAddress(bh.Host)
				return addr, bh.Listen, err
			}
		}
	case "mee":
		for _, mee := range profile.MEE {
			if mee.Name == inst.Name {
				addr, err := profile.HostAddress(mee.Host)
				return addr, mee.Listen, err
			}
		}
	}
	return "", 0, fmt.Errorf("listen endpoint not found for %s %s", inst.Role, inst.Name)
}

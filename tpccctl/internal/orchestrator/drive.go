package orchestrator

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"portable-tpcc/tpccctl/internal/collect"
	"portable-tpcc/tpccctl/internal/config"
	"portable-tpcc/tpccctl/internal/paths"
	"portable-tpcc/tpccctl/internal/remote"
	"portable-tpcc/tpccctl/internal/schedule"
	"portable-tpcc/tpccctl/internal/state"
)

// remoteRunDir is the per-run working directory on a runtime host.
func remoteRunDir(remoteRoot, runID string) string {
	return filepath.Join(remoteRoot, runID)
}

func (o *Orchestrator) dialConfig() (remote.DialConfig, error) {
	localRoot := o.Expanded.RemoteRoot
	abs, err := filepath.Abs(localRoot)
	if err != nil {
		return remote.DialConfig{}, err
	}
	return remote.DialConfigFromProfile(o.Profile, o.Expanded.KnownHosts, abs)
}

func (o *Orchestrator) openSessions() (map[string]remote.Session, error) {
	cfg, err := o.dialConfig()
	if err != nil {
		return nil, err
	}
	sessions := map[string]remote.Session{}
	for _, key := range remote.UniqueHostKeys(o.Profile) {
		entry, ok := o.Profile.Hosts[key]
		if !ok {
			return nil, fmt.Errorf("unknown host %q", key)
		}
		sess, err := remote.Dial(key, entry, cfg)
		if err != nil {
			for _, s := range sessions {
				_ = s.Close()
			}
			return nil, err
		}
		sessions[key] = sess
	}
	return sessions, nil
}

func closeSessions(sessions map[string]remote.Session) {
	for _, s := range sessions {
		_ = s.Close()
	}
}

func (o *Orchestrator) passwordEnv() map[string]string {
	name := o.Profile.Database.PasswordEnv
	if name == "" {
		return nil
	}
	val, ok := os.LookupEnv(name)
	if !ok {
		return nil
	}
	return map[string]string{name: val}
}

func (o *Orchestrator) binaryLocalPath() (string, error) {
	name := o.Opts.WorkerBinary
	if name == "" {
		name = filepath.Join(o.Expanded.LocalArtifacts, fmt.Sprintf("tpcc-%s", o.Profile.Database.DBMS))
	}
	if !filepath.IsAbs(name) {
		// Prefer explicit path; else look under local_artifacts.
		cand := filepath.Join(o.Expanded.LocalArtifacts, filepath.Base(name))
		if _, err := os.Stat(cand); err == nil {
			return cand, nil
		}
		if _, err := os.Stat(name); err == nil {
			abs, err := filepath.Abs(name)
			return abs, err
		}
		return cand, nil
	}
	return name, nil
}

func (o *Orchestrator) deployToHosts(ctx *Context, sessions map[string]remote.Session) error {
	binLocal, err := o.binaryLocalPath()
	if err != nil {
		return err
	}
	if _, err := os.Stat(binLocal); err != nil {
		return fmt.Errorf("worker binary not found at %s: %w", binLocal, err)
	}
	runConfigLocal := filepath.Join(ctx.RunDir, "run-config.json")
	binName := filepath.Base(binLocal)
	for hostKey, sess := range sessions {
		runDir := remoteRunDir(o.Expanded.RemoteRoot, ctx.RunID)
		if err := sess.MkdirAll(runDir); err != nil {
			return fmt.Errorf("host %s mkdir: %w", hostKey, err)
		}
		remoteBin := filepath.Join(runDir, binName)
		if err := sess.Upload(binLocal, remoteBin); err != nil {
			return fmt.Errorf("host %s upload binary: %w", hostKey, err)
		}
		// Ensure executable bit for local sessions (Upload sets 0755).
		remoteCfg := filepath.Join(runDir, "run-config.json")
		if err := sess.Upload(runConfigLocal, remoteCfg); err != nil {
			return fmt.Errorf("host %s upload run-config: %w", hostKey, err)
		}
	}
	return nil
}

type launchedProc struct {
	Role     string
	Host     string
	Instance string
	Session  remote.Session
	PID      int
	WorkDir  string
	ProcPath string // remote process.json path
	DonePath string // remote artifact-manifest.json path

	InstanceNonce string
}

func (o *Orchestrator) launchRole(
	ctx *Context,
	sessions map[string]remote.Session,
	role, hostKey, instance string,
	argv []string,
) (*launchedProc, error) {
	sess, ok := sessions[hostKey]
	if !ok {
		return nil, fmt.Errorf("no session for host %s", hostKey)
	}
	runDir := remoteRunDir(o.Expanded.RemoteRoot, ctx.RunID)
	instanceDir := filepath.Join(runDir, role, instance)
	if err := sess.MkdirAll(instanceDir); err != nil {
		return nil, err
	}
	binName := ctx.RunConfig.Binary
	remoteBin := filepath.Join(runDir, binName)
	stdout := filepath.Join(instanceDir, "stdout.log")
	stderr := filepath.Join(instanceDir, "stderr.log")
	env := o.passwordEnv()
	if env == nil && o.Profile.Database.PasswordEnv != "" {
		return nil, fmt.Errorf("environment variable %s is not set", o.Profile.Database.PasswordEnv)
	}
	pid, err := sess.StartDetached(runDir, remoteBin, argv, env, stdout, stderr)
	if err != nil {
		return nil, err
	}
	proc := &launchedProc{
		Role:     role,
		Host:     hostKey,
		Instance: instance,
		Session:  sess,
		PID:      pid,
		WorkDir:  runDir,
		ProcPath: filepath.Join(instanceDir, "process.json"),
		DonePath: filepath.Join(instanceDir, "artifact-manifest.json"),
	}
	_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
		Role:     role,
		Host:     hostKey,
		Instance: instance,
		PID:      pid,
		State:    "running",
	})
	_ = o.waitProcessMetadata(ctx, proc, 2*time.Second)
	return proc, nil
}

func (o *Orchestrator) waitProcessMetadata(ctx *Context, proc *launchedProc, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		loaded, err := o.loadProcessMetadata(ctx, proc)
		if loaded || err != nil || time.Now().After(deadline) {
			return err
		}
		time.Sleep(50 * time.Millisecond)
	}
}

func (o *Orchestrator) loadProcessMetadata(ctx *Context, proc *launchedProc) (bool, error) {
	exists, err := proc.Session.Exists(proc.ProcPath)
	if err != nil || !exists {
		return false, err
	}
	data, err := proc.Session.ReadFile(proc.ProcPath)
	if err != nil {
		return false, err
	}
	var meta map[string]interface{}
	if err := json.Unmarshal(data, &meta); err != nil {
		return false, err
	}
	if pid := remote.ParsePID(meta["pid"]); pid > 0 {
		proc.PID = pid
	}
	if nonce, ok := meta["instance_nonce"].(string); ok {
		proc.InstanceNonce = nonce
	}
	_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
		Role:          proc.Role,
		Host:          proc.Host,
		Instance:      proc.Instance,
		PID:           proc.PID,
		InstanceNonce: proc.InstanceNonce,
		State:         "running",
	})
	return true, nil
}

func (o *Orchestrator) waitProcesses(ctx *Context, procs []*launchedProc, timeout time.Duration, abortOnFail bool) error {
	deadline := time.Now().Add(timeout)
	remaining := map[string]*launchedProc{}
	for _, p := range procs {
		remaining[p.Role+"/"+p.Instance] = p
	}
	for len(remaining) > 0 {
		if time.Now().After(deadline) {
			return fmt.Errorf("timeout waiting for processes to finish")
		}
		for key, p := range remaining {
			if p.InstanceNonce == "" {
				if _, err := o.loadProcessMetadata(ctx, p); err != nil {
					return err
				}
			}
			done, err := p.Session.Exists(p.DonePath)
			if err != nil {
				return err
			}
			if done {
				data, err := p.Session.ReadFile(p.DonePath)
				if err != nil {
					return err
				}
				var manifest collect.ArtifactManifest
				if err := json.Unmarshal(data, &manifest); err != nil {
					return err
				}
				if p.InstanceNonce != "" && manifest.InstanceNonce != p.InstanceNonce {
					alive, err := p.Session.IsAlive(p.PID)
					if err == nil && !alive {
						_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
							Role: p.Role, Host: p.Host, Instance: p.Instance, PID: p.PID, InstanceNonce: p.InstanceNonce, State: "failed",
						})
						delete(remaining, key)
						if abortOnFail {
							return fmt.Errorf("%s/%s stale manifest nonce %q does not match launched nonce %q and process is dead", p.Role, p.Instance, manifest.InstanceNonce, p.InstanceNonce)
						}
					}
					continue
				}
				if !manifest.Finalized {
					alive, err := p.Session.IsAlive(p.PID)
					if err == nil && !alive {
						_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
							Role: p.Role, Host: p.Host, Instance: p.Instance, PID: p.PID, InstanceNonce: p.InstanceNonce, State: "failed",
						})
						delete(remaining, key)
						if abortOnFail {
							return fmt.Errorf("%s/%s died before finalizing artifacts", p.Role, p.Instance)
						}
					}
					continue
				}
				st := "exited"
				if manifest.ExitStatus != 0 {
					st = "failed"
				}
				_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
					Role: p.Role, Host: p.Host, Instance: p.Instance, PID: p.PID, InstanceNonce: p.InstanceNonce, State: st,
				})
				delete(remaining, key)
				if abortOnFail && manifest.ExitStatus != 0 {
					return fmt.Errorf("%s/%s exited with status %d", p.Role, p.Instance, manifest.ExitStatus)
				}
				continue
			}
			alive, err := p.Session.IsAlive(p.PID)
			if err == nil && !alive {
				// Process died before writing manifest.
				_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
					Role: p.Role, Host: p.Host, Instance: p.Instance, PID: p.PID, InstanceNonce: p.InstanceNonce, State: "failed",
				})
				delete(remaining, key)
				if abortOnFail {
					return fmt.Errorf("%s/%s died before finalizing artifacts", p.Role, p.Instance)
				}
			}
		}
		time.Sleep(500 * time.Millisecond)
	}
	return nil
}

func (o *Orchestrator) stopPeers(ctx *Context, sessions map[string]remote.Session) error {
	rs, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		return err
	}
	var first error
	for _, proc := range rs.Processes {
		if proc.State != "running" || proc.PID <= 0 {
			continue
		}
		sess, ok := sessions[proc.Host]
		if !ok {
			continue
		}
		if err := sess.Signal(proc.PID, "TERM"); err != nil && first == nil {
			first = err
		}
		_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
			Role: proc.Role, Host: proc.Host, Instance: proc.Instance, PID: proc.PID, State: "stopping",
		})
	}
	grace := time.Duration(ctx.RunConfig.Phases.StopGraceMs) * time.Millisecond
	if grace <= 0 {
		grace = 15 * time.Second
	}
	time.Sleep(grace)
	rs, _ = o.StateStore.Load(ctx.RunID)
	for _, proc := range rs.Processes {
		if proc.PID <= 0 {
			continue
		}
		sess, ok := sessions[proc.Host]
		if !ok {
			continue
		}
		alive, _ := sess.IsAlive(proc.PID)
		if alive {
			_ = sess.Signal(proc.PID, "KILL")
		}
	}
	return first
}

func (o *Orchestrator) superviseWorkers(ctx *Context, workers []*launchedProc, token schedule.Token, sessions map[string]remote.Session) error {
	measStart, err := schedule.ParseRFC3339(token.Phases.MeasurementStart)
	if err != nil {
		return err
	}
	drainEnd, err := schedule.ParseRFC3339(token.Phases.AsyncWorkDrainDeadline)
	if err != nil {
		return err
	}
	// Before measurement: any fatal aborts peers.
	for time.Now().Before(measStart) {
		for _, w := range workers {
			alive, _ := w.Session.IsAlive(w.PID)
			done, _ := w.Session.Exists(w.DonePath)
			if done {
				data, _ := w.Session.ReadFile(w.DonePath)
				var manifest map[string]interface{}
				_ = json.Unmarshal(data, &manifest)
				if v, ok := manifest["exit_status"].(float64); ok && int(v) != 0 {
					_ = o.stopPeers(ctx, sessions)
					return fmt.Errorf("worker %s failed before measurement", w.Instance)
				}
			}
			if !alive && !done {
				_ = o.stopPeers(ctx, sessions)
				return fmt.Errorf("worker %s died before measurement", w.Instance)
			}
		}
		_ = o.StateStore.Transition(ctx.RunID, state.StateArming)
		if time.Now().After(measStart.Add(-time.Duration(ctx.RunConfig.Phases.RampUpMs) * time.Millisecond)) {
			_ = o.StateStore.Transition(ctx.RunID, state.StateRamping)
		}
		time.Sleep(500 * time.Millisecond)
	}
	_ = o.StateStore.Transition(ctx.RunID, state.StateMeasuring)

	timeout := time.Until(drainEnd) + 2*time.Minute
	if timeout < time.Minute {
		timeout = time.Minute
	}
	if err := o.waitProcesses(ctx, workers, timeout, true); err != nil {
		_ = o.stopPeers(ctx, sessions)
		return err
	}
	return o.StateStore.Transition(ctx.RunID, state.StateDraining)
}

func (o *Orchestrator) collectArtifacts(ctx *Context, sessions map[string]remote.Session) error {
	collector := &collect.Collector{ResultRoot: o.Expanded.ResultRoot}
	var manifests []collect.ArtifactManifest

	collectOne := func(role, hostKey, instance string) error {
		sess := sessions[hostKey]
		runDir := remoteRunDir(o.Expanded.RemoteRoot, ctx.RunID)
		remoteInstance := filepath.Join(runDir, role, instance)
		localTmp := filepath.Join(o.Expanded.ResultRoot, ctx.RunID, ".collect-tmp", role, instance)
		_ = os.RemoveAll(localTmp)
		if err := os.MkdirAll(localTmp, 0755); err != nil {
			return err
		}
		// Pull known payload files listed in artifact-manifest when present.
		manifestRemote := filepath.Join(remoteInstance, "artifact-manifest.json")
		exists, err := sess.Exists(manifestRemote)
		if err != nil {
			return err
		}
		if !exists {
			return fmt.Errorf("missing artifact-manifest for %s/%s", role, instance)
		}
		data, err := sess.ReadFile(manifestRemote)
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(localTmp, "artifact-manifest.json"), data, 0644); err != nil {
			return err
		}
		var manifest collect.ArtifactManifest
		if err := json.Unmarshal(data, &manifest); err != nil {
			return err
		}
		for _, p := range manifest.Payloads {
			remotePath, err := paths.JoinUnder(remoteInstance, p.Path)
			if err != nil {
				return err
			}
			localPath, err := paths.JoinUnder(localTmp, p.Path)
			if err != nil {
				return err
			}
			if err := sess.Download(remotePath, localPath); err != nil {
				return fmt.Errorf("download %s: %w", remotePath, err)
			}
		}
		if err := collector.CollectInstance(ctx.RunID, role, instance, localTmp); err != nil {
			return err
		}
		manifests = append(manifests, manifest)
		return nil
	}

	for _, l := range ctx.RunConfig.LoadAssignment {
		if err := collectOne("loader", l.Host, l.Instance); err != nil {
			// Loaders may be skipped in some runs; keep going only if dir missing after skip.
			return err
		}
	}
	for _, w := range ctx.RunConfig.WorkerAssignment {
		if err := collectOne("worker", w.Host, w.Instance); err != nil {
			return err
		}
	}
	// Check reports live under runDir/checks/*.json
	checkHost := ctx.RunConfig.LoadAssignment[0].Host
	if len(ctx.RunConfig.LoadAssignment) == 0 && len(ctx.RunConfig.WorkerAssignment) > 0 {
		checkHost = ctx.RunConfig.WorkerAssignment[0].Host
	}
	if sess, ok := sessions[checkHost]; ok {
		runDir := remoteRunDir(o.Expanded.RemoteRoot, ctx.RunID)
		for _, phase := range []string{"after-import", "after-run"} {
			remotePath := filepath.Join(runDir, "checks", phase+".json")
			exists, _ := sess.Exists(remotePath)
			if !exists {
				continue
			}
			localDir := filepath.Join(o.Expanded.ResultRoot, ctx.RunID, "checks")
			if err := os.MkdirAll(localDir, 0755); err != nil {
				return err
			}
			if err := sess.Download(remotePath, filepath.Join(localDir, phase+".json")); err != nil {
				return err
			}
		}
	}

	// Promote orchestrator control artifacts into results layout.
	orchDir := filepath.Join(o.Expanded.ResultRoot, ctx.RunID, "orchestrator")
	if err := os.MkdirAll(orchDir, 0755); err != nil {
		return err
	}
	for _, name := range []string{"run-config.json", "profile.redacted.yaml", "run-state.json", "start-token.json", "orchestrator.log"} {
		src := filepath.Join(ctx.RunDir, name)
		if _, err := os.Stat(src); err != nil {
			continue
		}
		data, err := os.ReadFile(src)
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(orchDir, name), data, 0644); err != nil {
			return err
		}
	}
	return collector.WriteCollectionManifest(ctx.RunID, manifests, nil)
}

func hostForLoader(rc *config.RunConfig, instance string) string {
	for _, l := range rc.LoadAssignment {
		if l.Instance == instance {
			return l.Host
		}
	}
	return ""
}

func hostForWorker(rc *config.RunConfig, instance string) string {
	for _, w := range rc.WorkerAssignment {
		if w.Instance == instance {
			return w.Host
		}
	}
	return ""
}

package orchestrator

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"portable-tpcc/mind/internal/collect"
	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/paths"
	"portable-tpcc/mind/internal/progress"
	"portable-tpcc/mind/internal/remote"
	"portable-tpcc/mind/internal/schedule"
	"portable-tpcc/mind/internal/state"
)

func (o *Orchestrator) checkInterrupted() error {
	if o == nil || o.Opts.Interrupt == nil {
		return nil
	}
	select {
	case <-o.Opts.Interrupt.Done():
		return ErrInterrupted
	default:
		return nil
	}
}

func (o *Orchestrator) sleep(d time.Duration) error {
	if err := o.checkInterrupted(); err != nil {
		return err
	}
	if o == nil || o.Opts.Interrupt == nil {
		time.Sleep(d)
		return nil
	}
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-o.Opts.Interrupt.Done():
		return ErrInterrupted
	case <-timer.C:
		return nil
	}
}

// remoteRunDir is the per-run working directory on a runtime host.
func remoteRunDir(remoteRoot, runID string) string {
	return filepath.Join(remoteRoot, runID)
}

// runtimeRoot returns paths.remote_root as it should be used on sess.
// Local loopback expands ~/ and resolves against the control-host cwd; SSH keeps
// the host-native form (relative, absolute, or ~/ on the remote account).
func (o *Orchestrator) runtimeRoot(sess remote.Session) (string, error) {
	root := o.Expanded.RemoteRoot
	if _, ok := sess.(*remote.Local); !ok {
		return root, nil
	}
	expanded, err := paths.ExpandHome(root)
	if err != nil {
		return "", err
	}
	abs, err := filepath.Abs(expanded)
	if err != nil {
		return "", err
	}
	return abs, nil
}

func (o *Orchestrator) sessionRunDir(sess remote.Session, runID string) (string, error) {
	root, err := o.runtimeRoot(sess)
	if err != nil {
		return "", err
	}
	return remoteRunDir(root, runID), nil
}

func (o *Orchestrator) dialConfig() (remote.DialConfig, error) {
	localRoot, err := paths.ExpandHome(o.Expanded.RemoteRoot)
	if err != nil {
		return remote.DialConfig{}, err
	}
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
	hosts := remote.UniqueHosts(o.Profile)
	progress.Printf("connecting to %d runtime host(s)", len(hosts))
	sessions := map[string]remote.Session{}
	for _, host := range hosts {
		progress.Printf("connect %s", host)
		sess, err := remote.Dial(host, cfg)
		if err != nil {
			for _, s := range sessions {
				_ = s.Close()
			}
			return nil, err
		}
		sessions[host] = sess
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

func (o *Orchestrator) requiresPasswordEnv() bool {
	if o.Profile.Database.PasswordEnv == "" {
		return false
	}
	if o.Profile.Database.DBMS == "ydb" {
		return config.InferYdbAuthScheme(o.Profile.Database) == "login"
	}
	return true
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
	credFiles, err := o.credentialFilesForDeploy()
	if err != nil {
		return err
	}
	binName := filepath.Base(binLocal)
	progress.Printf("deploying %s to %d host(s)", binName, len(sessions))
	for hostKey, sess := range sessions {
		runDir, err := o.sessionRunDir(sess, ctx.RunID)
		if err != nil {
			return fmt.Errorf("host %s remote_root: %w", hostKey, err)
		}
		progress.Printf("deploy %s: mkdir %s", hostKey, runDir)
		if err := sess.MkdirAll(runDir); err != nil {
			return fmt.Errorf("host %s mkdir: %w", hostKey, err)
		}
		remoteBin := filepath.Join(runDir, binName)
		progress.Printf("deploy %s: upload binary", hostKey)
		if err := sess.Upload(binLocal, remoteBin); err != nil {
			return fmt.Errorf("host %s upload binary: %w", hostKey, err)
		}
		// Upload sets mode 0755 (local OpenFile / SSH chmod) so the binary is executable.
		remoteCfg := filepath.Join(runDir, "run-config.json")
		progress.Printf("deploy %s: upload run-config.json", hostKey)
		if err := sess.Upload(runConfigLocal, remoteCfg); err != nil {
			return fmt.Errorf("host %s upload run-config: %w", hostKey, err)
		}
		for _, f := range credFiles {
			remotePath := filepath.Join(runDir, f.RemoteName)
			progress.Printf("deploy %s: upload %s", hostKey, f.RemoteName)
			if err := sess.Upload(f.LocalPath, remotePath); err != nil {
				return fmt.Errorf("host %s upload %s: %w", hostKey, f.RemoteName, err)
			}
		}
		progress.Printf("deploy %s: done", hostKey)
	}
	return nil
}

type credentialFile struct {
	LocalPath  string
	RemoteName string
}

// credentialFilesForDeploy resolves control-host CA / SA-key paths that must be
// uploaded beside run-config.json. Remote names match rewritten run-config fields.
func (o *Orchestrator) credentialFilesForDeploy() ([]credentialFile, error) {
	var out []credentialFile
	if o.Profile.Database.CaFile != "" {
		local, err := paths.ExpandHome(o.Profile.Database.CaFile)
		if err != nil {
			return nil, fmt.Errorf("database.ca_file: %w", err)
		}
		if !filepath.IsAbs(local) {
			abs, err := filepath.Abs(local)
			if err != nil {
				return nil, fmt.Errorf("database.ca_file: %w", err)
			}
			local = abs
		}
		if _, err := os.Stat(local); err != nil {
			return nil, fmt.Errorf("database.ca_file not found at %s: %w", local, err)
		}
		out = append(out, credentialFile{LocalPath: local, RemoteName: config.RemoteCAFileName})
	}
	if o.Profile.Database.SaKeyFile != "" {
		local, err := paths.ExpandHome(o.Profile.Database.SaKeyFile)
		if err != nil {
			return nil, fmt.Errorf("database.sa_key_file: %w", err)
		}
		if !filepath.IsAbs(local) {
			abs, err := filepath.Abs(local)
			if err != nil {
				return nil, fmt.Errorf("database.sa_key_file: %w", err)
			}
			local = abs
		}
		if _, err := os.Stat(local); err != nil {
			return nil, fmt.Errorf("database.sa_key_file not found at %s: %w", local, err)
		}
		out = append(out, credentialFile{LocalPath: local, RemoteName: config.RemoteSAKeyFileName})
	}
	return out, nil
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
	runDir, err := o.sessionRunDir(sess, ctx.RunID)
	if err != nil {
		return nil, fmt.Errorf("host %s remote_root: %w", hostKey, err)
	}
	instanceDir := filepath.Join(runDir, role, instance)
	if err := sess.MkdirAll(instanceDir); err != nil {
		return nil, err
	}
	binName := ctx.RunConfig.Binary
	remoteBin := filepath.Join(runDir, binName)
	stdout := filepath.Join(instanceDir, "stdout.log")
	stderr := filepath.Join(instanceDir, "stderr.log")
	exists, err := sess.Exists(remoteBin)
	if err != nil {
		return nil, fmt.Errorf("check binary %s on %s: %w", remoteBin, hostKey, err)
	}
	if !exists {
		return nil, fmt.Errorf("worker binary %s not found on %s; run `mind-tpcc deploy --profile ... --run-id %s` first", remoteBin, hostKey, ctx.RunID)
	}
	var env map[string]string
	if o.requiresPasswordEnv() {
		env = o.passwordEnv()
		if env == nil {
			return nil, fmt.Errorf("environment variable %s is not set", o.Profile.Database.PasswordEnv)
		}
	}
	progress.Printf("launch %s/%s on %s", role, instance, hostKey)
	pid, err := sess.StartDetached(runDir, remoteBin, argv, env, stdout, stderr)
	if err != nil {
		return nil, err
	}
	progress.Printf("launch %s/%s: pid %d", role, instance, pid)
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
	if err := o.waitProcessMetadata(ctx, proc, 2*time.Second); err != nil {
		_ = sess.Signal(proc.PID, "TERM")
		_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
			Role:          role,
			Host:          hostKey,
			Instance:      instance,
			PID:           proc.PID,
			InstanceNonce: proc.InstanceNonce,
			State:         "failed",
		})
		return nil, err
	}
	progress.Printf("launch %s/%s: process metadata ready (nonce=%s)", role, instance, proc.InstanceNonce)
	return proc, nil
}

func (o *Orchestrator) waitProcessMetadata(ctx *Context, proc *launchedProc, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		loaded, err := o.loadProcessMetadata(ctx, proc)
		if err != nil {
			return err
		}
		if loaded {
			if proc.InstanceNonce == "" {
				return fmt.Errorf("%s/%s process metadata missing instance_nonce", proc.Role, proc.Instance)
			}
			return nil
		}
		if time.Now().After(deadline) {
			return o.processMetadataTimeoutErr(proc)
		}
		if err := o.sleep(50 * time.Millisecond); err != nil {
			return err
		}
	}
}

func (o *Orchestrator) processMetadataTimeoutErr(proc *launchedProc) error {
	var b strings.Builder
	fmt.Fprintf(&b, "timeout waiting for %s/%s process metadata at %s", proc.Role, proc.Instance, proc.ProcPath)
	if proc.PID > 0 {
		alive, err := proc.Session.IsAlive(proc.PID)
		if err != nil {
			fmt.Fprintf(&b, " (pid %d alive=?: %v)", proc.PID, err)
		} else {
			fmt.Fprintf(&b, " (pid %d alive=%v)", proc.PID, alive)
		}
	}
	return o.withProcessLogs(proc, b.String())
}

// withProcessLogs appends stderr/stdout tails from the instance directory.
func (o *Orchestrator) withProcessLogs(proc *launchedProc, msg string) error {
	var b strings.Builder
	b.WriteString(msg)
	instanceDir := processInstanceDir(proc)
	for _, name := range []string{"stderr.log", "stdout.log"} {
		path := filepath.Join(instanceDir, name)
		data, err := proc.Session.ReadFile(path)
		if err != nil || len(bytes.TrimSpace(data)) == 0 {
			continue
		}
		const max = 2048
		if len(data) > max {
			data = data[len(data)-max:]
		}
		fmt.Fprintf(&b, "\n---- %s ----\n%s", name, strings.TrimSpace(string(data)))
	}
	return fmt.Errorf("%s", b.String())
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

type processLogCursor struct {
	stderrOff int
	stdoutOff int
}

func processInstanceDir(proc *launchedProc) string {
	instanceDir := filepath.Dir(proc.ProcPath)
	if instanceDir == "" || instanceDir == "." {
		instanceDir = filepath.Dir(proc.DonePath)
	}
	return instanceDir
}

// completeLogLines returns finished lines from data[off:] and the new offset
// (advanced only past complete newline-terminated lines).
func completeLogLines(data []byte, off int) (lines []string, newOff int) {
	if off < 0 || off > len(data) {
		off = 0
	}
	newOff = off
	for newOff < len(data) {
		i := bytes.IndexByte(data[newOff:], '\n')
		if i < 0 {
			break
		}
		line := string(bytes.TrimRight(data[newOff:newOff+i], "\r"))
		newOff += i + 1
		if line != "" {
			lines = append(lines, line)
		}
	}
	return lines, newOff
}

func (o *Orchestrator) relayProcessLogs(proc *launchedProc, curs *processLogCursor) bool {
	instanceDir := processInstanceDir(proc)
	relayed := false
	for _, item := range []struct {
		name string
		off  *int
	}{
		{"stderr.log", &curs.stderrOff},
		{"stdout.log", &curs.stdoutOff},
	} {
		path := filepath.Join(instanceDir, item.name)
		data, err := proc.Session.ReadFile(path)
		if err != nil {
			continue
		}
		lines, newOff := completeLogLines(data, *item.off)
		*item.off = newOff
		for _, line := range lines {
			progress.Printf("[%s/%s] %s", proc.Role, proc.Instance, line)
			relayed = true
		}
	}
	return relayed
}

func (o *Orchestrator) waitProcesses(ctx *Context, procs []*launchedProc, timeout time.Duration, abortOnFail bool) error {
	deadline := time.Now().Add(timeout)
	started := time.Now()
	remaining := map[string]*launchedProc{}
	cursors := map[string]*processLogCursor{}
	for _, p := range procs {
		key := p.Role + "/" + p.Instance
		remaining[key] = p
		cursors[key] = &processLogCursor{}
	}
	if len(remaining) > 0 {
		progress.Printf("waiting for %d process(es): %s", len(remaining), sortedKeys(remaining))
	}
	lastHeartbeat := time.Now()
	const heartbeatEvery = 15 * time.Second
	for len(remaining) > 0 {
		if err := o.checkInterrupted(); err != nil {
			progress.Printf("interrupted; stopping process wait so profile lock can be released")
			return err
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("timeout waiting for processes to finish")
		}
		anyRelayed := false
		for key, p := range remaining {
			if o.relayProcessLogs(p, cursors[key]) {
				anyRelayed = true
			}
			if p.InstanceNonce == "" {
				loaded, err := o.loadProcessMetadata(ctx, p)
				if err != nil {
					return err
				}
				if !loaded || p.InstanceNonce == "" {
					return fmt.Errorf("%s/%s process metadata missing instance_nonce", p.Role, p.Instance)
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
						_ = o.relayProcessLogs(p, cursors[key])
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
						_ = o.relayProcessLogs(p, cursors[key])
						delete(remaining, key)
						if abortOnFail {
							return o.withProcessLogs(p, fmt.Sprintf("%s/%s died before finalizing artifacts", p.Role, p.Instance))
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
				_ = o.relayProcessLogs(p, cursors[key])
				progress.Printf("%s/%s finished (%s, exit=%d)", p.Role, p.Instance, st, manifest.ExitStatus)
				delete(remaining, key)
				if abortOnFail && manifest.ExitStatus != 0 {
					return o.withProcessLogs(p, fmt.Sprintf("%s/%s exited with status %d", p.Role, p.Instance, manifest.ExitStatus))
				}
				continue
			}
			alive, err := p.Session.IsAlive(p.PID)
			if err == nil && !alive {
				// Process died before writing manifest.
				_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
					Role: p.Role, Host: p.Host, Instance: p.Instance, PID: p.PID, InstanceNonce: p.InstanceNonce, State: "failed",
				})
				_ = o.relayProcessLogs(p, cursors[key])
				delete(remaining, key)
				if abortOnFail {
					return o.withProcessLogs(p, fmt.Sprintf("%s/%s died before finalizing artifacts", p.Role, p.Instance))
				}
			}
		}
		if !anyRelayed && time.Since(lastHeartbeat) >= heartbeatEvery && len(remaining) > 0 {
			progress.Printf("still waiting for %d process(es) after %s: %s",
				len(remaining), time.Since(started).Round(time.Second), sortedKeys(remaining))
			lastHeartbeat = time.Now()
		}
		if anyRelayed {
			lastHeartbeat = time.Now()
		}
		if err := o.sleep(500 * time.Millisecond); err != nil {
			progress.Printf("interrupted; stopping process wait so profile lock can be released")
			return err
		}
	}
	return nil
}

func sortedKeys(m map[string]*launchedProc) string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return strings.Join(keys, ", ")
}

func (o *Orchestrator) stopPeers(ctx *Context, sessions map[string]remote.Session) error {
	rs, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		return err
	}
	var first error
	stopping := 0
	for _, proc := range rs.Processes {
		if proc.State != "running" || proc.PID <= 0 {
			continue
		}
		sess, ok := sessions[proc.Host]
		if !ok {
			continue
		}
		progress.Printf("stop %s/%s pid %d (TERM)", proc.Role, proc.Instance, proc.PID)
		if err := sess.Signal(proc.PID, "TERM"); err != nil && first == nil {
			first = err
		}
		_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
			Role: proc.Role, Host: proc.Host, Instance: proc.Instance, PID: proc.PID, State: "stopping",
		})
		stopping++
	}
	grace := time.Duration(ctx.RunConfig.Phases.StopGraceMs) * time.Millisecond
	if grace <= 0 {
		grace = 15 * time.Second
	}
	if stopping > 0 {
		progress.Printf("waiting %s stop grace for %d process(es)", grace, stopping)
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
	cursors := map[string]*processLogCursor{}
	for _, w := range workers {
		cursors[w.Role+"/"+w.Instance] = &processLogCursor{}
	}
	progress.Printf("workers armed; measurement starts at %s", token.Phases.MeasurementStart)
	lastHeartbeat := time.Now()
	const heartbeatEvery = 15 * time.Second
	// Before measurement: any fatal aborts peers.
	for time.Now().Before(measStart) {
		if err := o.checkInterrupted(); err != nil {
			_ = o.stopPeers(ctx, sessions)
			return err
		}
		anyRelayed := false
		for _, w := range workers {
			key := w.Role + "/" + w.Instance
			if o.relayProcessLogs(w, cursors[key]) {
				anyRelayed = true
			}
			alive, _ := w.Session.IsAlive(w.PID)
			done, _ := w.Session.Exists(w.DonePath)
			if done {
				data, _ := w.Session.ReadFile(w.DonePath)
				var manifest map[string]interface{}
				_ = json.Unmarshal(data, &manifest)
				if v, ok := manifest["exit_status"].(float64); ok && int(v) != 0 {
					_ = o.relayProcessLogs(w, cursors[key])
					_ = o.stopPeers(ctx, sessions)
					return fmt.Errorf("worker %s failed before measurement", w.Instance)
				}
			}
			if !alive && !done {
				_ = o.relayProcessLogs(w, cursors[key])
				_ = o.stopPeers(ctx, sessions)
				return fmt.Errorf("worker %s died before measurement", w.Instance)
			}
		}
		_ = o.StateStore.Transition(ctx.RunID, state.StateArming)
		if time.Now().After(measStart.Add(-time.Duration(ctx.RunConfig.Phases.RampUpMs) * time.Millisecond)) {
			_ = o.StateStore.Transition(ctx.RunID, state.StateRamping)
		}
		if !anyRelayed && time.Since(lastHeartbeat) >= heartbeatEvery {
			progress.Printf("waiting for measurement start in %s", time.Until(measStart).Round(time.Second))
			lastHeartbeat = time.Now()
		}
		if anyRelayed {
			lastHeartbeat = time.Now()
		}
		if err := o.sleep(500 * time.Millisecond); err != nil {
			_ = o.stopPeers(ctx, sessions)
			return err
		}
	}
	progress.Printf("measurement phase started")
	_ = o.StateStore.Transition(ctx.RunID, state.StateMeasuring)

	timeout := time.Until(drainEnd) + 2*time.Minute
	if timeout < time.Minute {
		timeout = time.Minute
	}
	if err := o.waitProcesses(ctx, workers, timeout, true); err != nil {
		_ = o.stopPeers(ctx, sessions)
		return err
	}
	progress.Printf("workers finished; entering drain")
	return o.StateStore.Transition(ctx.RunID, state.StateDraining)
}

type realpathSession interface {
	Realpath(remotePath string) (string, error)
}

func validateRemotePathUnder(sess remote.Session, base, elem string) error {
	if _, ok := sess.(*remote.Local); ok {
		if _, err := paths.ResolveUnder(base, elem); err != nil {
			return err
		}
		return nil
	}
	rp, ok := sess.(realpathSession)
	if !ok {
		return nil
	}
	target, err := paths.JoinRelative(base, elem)
	if err != nil {
		return err
	}
	baseReal, err := rp.Realpath(base)
	if err != nil {
		return err
	}
	targetReal, err := rp.Realpath(target)
	if err != nil {
		return err
	}
	baseClean := filepath.Clean(baseReal)
	targetClean := filepath.Clean(targetReal)
	if targetClean != baseClean && !strings.HasPrefix(targetClean, baseClean+"/") {
		return fmt.Errorf("resolved path %q escapes base %q", targetClean, baseClean)
	}
	return nil
}

func (o *Orchestrator) collectArtifacts(ctx *Context, sessions map[string]remote.Session) error {
	collector := &collect.Collector{ResultRoot: o.Expanded.ResultRoot}
	var manifests []collect.ArtifactManifest

	collectOne := func(role, hostKey, instance string) error {
		progress.Printf("collect %s/%s from %s", role, instance, hostKey)
		sess := sessions[hostKey]
		runDir, err := o.sessionRunDir(sess, ctx.RunID)
		if err != nil {
			return err
		}
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
		if err := validateRemotePathUnder(sess, remoteInstance, "artifact-manifest.json"); err != nil {
			return err
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
			var remotePath string
			if _, ok := sess.(*remote.Local); ok {
				remotePath, err = paths.JoinUnder(remoteInstance, p.Path)
			} else {
				remotePath, err = paths.JoinRelative(remoteInstance, p.Path)
			}
			if err != nil {
				return err
			}
			if err := collect.ValidateArtifactPayloadPath(p.Path); err != nil {
				return err
			}
			if err := validateRemotePathUnder(sess, remoteInstance, p.Path); err != nil {
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
		progress.Printf("collect %s/%s: done (%d payload(s))", role, instance, len(manifest.Payloads))
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
		runDir, err := o.sessionRunDir(sess, ctx.RunID)
		if err != nil {
			return err
		}
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

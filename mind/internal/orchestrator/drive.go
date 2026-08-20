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

// remoteBinaryPath is the shared worker binary location on a runtime host.
// It lives directly under paths.remote_root so successive runs can reuse one
// deploy without re-uploading into each run_id directory.
func remoteBinaryPath(remoteRoot, binName string) string {
	return filepath.Join(remoteRoot, filepath.Base(binName))
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

func (o *Orchestrator) sessionBinaryPath(sess remote.Session, binName string) (string, error) {
	root, err := o.runtimeRoot(sess)
	if err != nil {
		return "", err
	}
	return remoteBinaryPath(root, binName), nil
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

func (o *Orchestrator) controlHostPassword() (string, error) {
	name := o.Profile.Database.PasswordEnv
	if name == "" {
		return "", fmt.Errorf("database.password_env is not set in the profile")
	}
	val, ok := os.LookupEnv(name)
	if !ok {
		return "", fmt.Errorf("environment variable %s is not set", name)
	}
	return val, nil
}

func (o *Orchestrator) requiresPasswordFile() bool {
	return config.NeedsRemotePasswordFile(o.Profile.Database)
}

// ensureRemotePasswordFile writes the control-host password_env value to the
// worker-local db-password file (mode 0600). The secret is never placed in the
// SSH/nohup command line.
func (o *Orchestrator) ensureRemotePasswordFile(sess remote.Session, runDir string) error {
	if !o.requiresPasswordFile() {
		return nil
	}
	password, err := o.controlHostPassword()
	if err != nil {
		return err
	}
	remotePath := filepath.Join(runDir, config.RemotePasswordFileName)
	if err := sess.WriteFileMode(remotePath, []byte(password), 0600); err != nil {
		return fmt.Errorf("write %s: %w", config.RemotePasswordFileName, err)
	}
	return nil
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

func (o *Orchestrator) deployToHosts(sessions map[string]remote.Session) error {
	binLocal, err := o.binaryLocalPath()
	if err != nil {
		return err
	}
	if _, err := os.Stat(binLocal); err != nil {
		return fmt.Errorf("worker binary not found at %s: %w", binLocal, err)
	}
	binName := filepath.Base(binLocal)
	progress.Printf("deploying %s to %d host(s)", binName, len(sessions))
	for hostKey, sess := range sessions {
		root, err := o.runtimeRoot(sess)
		if err != nil {
			return fmt.Errorf("host %s remote_root: %w", hostKey, err)
		}
		// Shared worker binary under remote_root. Per-run run-config.json and
		// credentials are pushed at launch time for the active run_id.
		if err := sess.MkdirAll(root); err != nil {
			return fmt.Errorf("host %s mkdir: %w", hostKey, err)
		}
		remoteBin := remoteBinaryPath(root, binName)
		exists, err := sess.Exists(remoteBin)
		if err != nil {
			return fmt.Errorf("host %s check binary: %w", hostKey, err)
		}
		if exists {
			progress.Printf("deploy %s: remove existing %s", hostKey, remoteBin)
			if err := sess.Remove(remoteBin); err != nil {
				return fmt.Errorf("host %s remove existing binary: %w", hostKey, err)
			}
		}
		progress.Printf("deploy %s: upload binary %s", hostKey, remoteBin)
		if err := sess.Upload(binLocal, remoteBin); err != nil {
			return fmt.Errorf("host %s upload binary: %w", hostKey, err)
		}
		// Upload sets mode 0755 (local OpenFile / SSH chmod) so the binary is executable.
		progress.Printf("deploy %s: done", hostKey)
	}
	return nil
}

func (o *Orchestrator) undeployFromHosts(sessions map[string]remote.Session) error {
	binLocal, err := o.binaryLocalPath()
	if err != nil {
		return err
	}
	binName := filepath.Base(binLocal)
	progress.Printf("undeploying %s from %d host(s)", binName, len(sessions))
	for hostKey, sess := range sessions {
		root, err := o.runtimeRoot(sess)
		if err != nil {
			return fmt.Errorf("host %s remote_root: %w", hostKey, err)
		}
		remoteBin := remoteBinaryPath(root, binName)
		exists, err := sess.Exists(remoteBin)
		if err != nil {
			return fmt.Errorf("host %s check binary: %w", hostKey, err)
		}
		if !exists {
			progress.Printf("undeploy %s: binary absent (%s)", hostKey, remoteBin)
			continue
		}
		progress.Printf("undeploy %s: remove %s", hostKey, remoteBin)
		if err := sess.Remove(remoteBin); err != nil {
			return fmt.Errorf("host %s remove binary: %w", hostKey, err)
		}
	}
	return nil
}

// workerBinaryMissingHosts returns hosts where the shared worker binary is absent.
func (o *Orchestrator) workerBinaryMissingHosts(sessions map[string]remote.Session) (binName string, missing []string, err error) {
	binLocal, err := o.binaryLocalPath()
	if err != nil {
		return "", nil, err
	}
	binName = filepath.Base(binLocal)
	for hostKey, sess := range sessions {
		remoteBin, err := o.sessionBinaryPath(sess, binName)
		if err != nil {
			return "", nil, fmt.Errorf("host %s remote_root: %w", hostKey, err)
		}
		exists, err := sess.Exists(remoteBin)
		if err != nil {
			return "", nil, fmt.Errorf("host %s check binary: %w", hostKey, err)
		}
		if !exists {
			missing = append(missing, hostKey)
		}
	}
	sort.Strings(missing)
	return binName, missing, nil
}

// ensureRemoteRunFiles uploads run-config.json and DB credential files into the
// per-run working directory on a runtime host. Called before every role launch
// so stages work without a preceding deploy of run-scoped artifacts.
func (o *Orchestrator) ensureRemoteRunFiles(ctx *Context, sess remote.Session, runDir string) error {
	if ctx == nil || ctx.RunDir == "" {
		return fmt.Errorf("run directory is not set")
	}
	if err := sess.MkdirAll(runDir); err != nil {
		return fmt.Errorf("mkdir %s: %w", runDir, err)
	}
	localCfg := filepath.Join(ctx.RunDir, "run-config.json")
	if _, err := os.Stat(localCfg); err != nil {
		return fmt.Errorf("local run-config.json: %w", err)
	}
	remoteCfg := filepath.Join(runDir, "run-config.json")
	progress.Printf("upload run-config.json -> %s", remoteCfg)
	if err := sess.Upload(localCfg, remoteCfg); err != nil {
		return fmt.Errorf("upload run-config: %w", err)
	}
	credFiles, err := o.credentialFilesForRun()
	if err != nil {
		return err
	}
	for _, f := range credFiles {
		remotePath := filepath.Join(runDir, f.RemoteName)
		progress.Printf("upload %s -> %s", f.RemoteName, remotePath)
		if err := sess.Upload(f.LocalPath, remotePath); err != nil {
			return fmt.Errorf("upload %s: %w", f.RemoteName, err)
		}
	}
	if err := o.ensureRemotePasswordFile(sess, runDir); err != nil {
		return err
	}
	return nil
}

type credentialFile struct {
	LocalPath  string
	RemoteName string
}

// credentialFilesForRun resolves control-host CA / SA-key paths that must be
// uploaded beside run-config.json. Remote names match rewritten run-config fields.
func (o *Orchestrator) credentialFilesForRun() ([]credentialFile, error) {
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
	Role      string
	Host      string
	Instance  string
	Session   remote.Session
	PID       int
	LaunchPID int // PID from StartDetached (wrapper shell); never overwritten
	WorkDir   string
	ProcPath  string // remote process.json path
	DonePath  string // remote artifact-manifest.json path
	Argv      []string

	InstanceNonce string
	// Finished is set when waitProcesses accepted a finalized artifact manifest.
	Finished bool
	// warnedAlive is set after logging that the process was still alive after Finished.
	warnedAlive bool
}

func uniquePIDs(pids ...int) []int {
	seen := map[int]struct{}{}
	var out []int
	for _, pid := range pids {
		if pid <= 0 {
			continue
		}
		if _, ok := seen[pid]; ok {
			continue
		}
		seen[pid] = struct{}{}
		out = append(out, pid)
	}
	return out
}

func (p *launchedProc) pids() []int {
	if p == nil {
		return nil
	}
	return uniquePIDs(p.LaunchPID, p.PID)
}

func (p *launchedProc) anyAlive() (bool, int, error) {
	if p == nil || p.Session == nil {
		return false, 0, nil
	}
	var lastErr error
	for _, pid := range p.pids() {
		alive, err := p.Session.IsAlive(pid)
		if err != nil {
			lastErr = err
			continue
		}
		if alive {
			return true, pid, nil
		}
	}
	return false, 0, lastErr
}

func signalPIDs(sess remote.Session, pids []int, sig string) {
	if sess == nil {
		return
	}
	for _, pid := range pids {
		_ = sess.Signal(pid, sig)
	}
}

func (o *Orchestrator) trackLaunch(proc *launchedProc) {
	if o == nil || proc == nil {
		return
	}
	o.launched = append(o.launched, proc)
}

func (o *Orchestrator) clearStaleInstanceMetadata(sess remote.Session, instanceDir string) {
	if sess == nil || instanceDir == "" {
		return
	}
	var cleared []string
	for _, name := range []string{"process.json", "ready.json", "result.json", "artifact-manifest.json"} {
		path := filepath.Join(instanceDir, name)
		exists, err := sess.Exists(path)
		if err != nil || !exists {
			continue
		}
		if err := sess.Remove(path); err != nil {
			progress.Printf("warning: could not remove stale %s: %v", path, err)
			continue
		}
		cleared = append(cleared, name)
	}
	if len(cleared) > 0 {
		progress.Printf("cleared stale instance metadata: %s", strings.Join(cleared, ", "))
	}
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
	remoteBin, err := o.sessionBinaryPath(sess, ctx.RunConfig.Binary)
	if err != nil {
		return nil, fmt.Errorf("host %s remote_root: %w", hostKey, err)
	}
	stdout := filepath.Join(instanceDir, "stdout.log")
	stderr := filepath.Join(instanceDir, "stderr.log")
	exists, err := sess.Exists(remoteBin)
	if err != nil {
		return nil, fmt.Errorf("check binary %s on %s: %w", remoteBin, hostKey, err)
	}
	if !exists {
		return nil, fmt.Errorf("worker binary %s not found on %s; run `mind-tpcc deploy --profile ...` first", remoteBin, hostKey)
	}
	// Push per-run run-config + credentials on every launch so a new run_id
	// does not require redeploy. Password is written to a mode-0600 file and
	// never injected into argv/env of the remote shell command (visible in ps).
	if err := o.ensureRemoteRunFiles(ctx, sess, runDir); err != nil {
		return nil, fmt.Errorf("host %s: %w", hostKey, err)
	}
	// Drop leftover process.json / artifact-manifest.json from a previous
	// attempt so waitProcesses cannot treat the old nonce as this launch.
	o.clearStaleInstanceMetadata(sess, instanceDir)
	progress.Printf("launch %s/%s on %s", role, instance, hostKey)
	pid, err := sess.StartDetached(runDir, remoteBin, argv, nil, stdout, stderr)
	if err != nil {
		return nil, err
	}
	progress.Printf("launch %s/%s: pid %d", role, instance, pid)
	proc := &launchedProc{
		Role:      role,
		Host:      hostKey,
		Instance:  instance,
		Session:   sess,
		PID:       pid,
		LaunchPID: pid,
		WorkDir:   runDir,
		ProcPath:  filepath.Join(instanceDir, "process.json"),
		DonePath:  filepath.Join(instanceDir, "artifact-manifest.json"),
		Argv:      append([]string(nil), argv...),
	}
	o.trackLaunch(proc)
	_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
		Role:      role,
		Host:      hostKey,
		Instance:  instance,
		PID:       pid,
		LaunchPID: pid,
		State:     "running",
	})
	if err := o.waitProcessMetadata(ctx, proc, 2*time.Second); err != nil {
		signalPIDs(sess, proc.pids(), "TERM")
		_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
			Role:          role,
			Host:          hostKey,
			Instance:      instance,
			PID:           proc.PID,
			LaunchPID:     proc.LaunchPID,
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

// withProcessLogs appends check-report failures and stderr/stdout tails.
func (o *Orchestrator) withProcessLogs(proc *launchedProc, msg string) error {
	var b strings.Builder
	b.WriteString(msg)
	diag := checkFailureDiagnostics(proc)
	if diag != "" {
		fmt.Fprintf(&b, "\n%s", diag)
	}
	instanceDir := processInstanceDir(proc)
	for _, name := range []string{"stderr.log", "stdout.log"} {
		if name == "stdout.log" && diag != "" {
			// Structured check failures already cover stdout; the tail is mostly [Skipped].
			continue
		}
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

func checkPhaseFromArgv(argv []string) string {
	for _, a := range argv {
		switch a {
		case "--after-import":
			return "after-import"
		case "--after-run":
			return "after-run"
		}
	}
	return ""
}

type checkReportJSON struct {
	Phase   string `json:"phase"`
	OK      bool   `json:"ok"`
	Passed  int    `json:"passed"`
	Failed  int    `json:"failed"`
	Skipped int    `json:"skipped"`
	Errors  int    `json:"errors"`
	Checks  []struct {
		ID     string `json:"id"`
		Title  string `json:"title"`
		Status string `json:"status"`
		Detail string `json:"detail"`
	} `json:"checks"`
}

func formatCheckReport(data []byte) string {
	var report checkReportJSON
	if err := json.Unmarshal(data, &report); err != nil {
		return ""
	}
	if report.OK && report.Failed == 0 && report.Errors == 0 {
		return ""
	}
	phase := report.Phase
	if phase == "" {
		phase = "check"
	}
	var b strings.Builder
	fmt.Fprintf(&b, "check %s: %d passed, %d failed, %d skipped, %d errors",
		phase, report.Passed, report.Failed, report.Skipped, report.Errors)
	for _, c := range report.Checks {
		if c.Status != "failed" && c.Status != "error" {
			continue
		}
		title := c.Title
		if title == "" {
			title = c.ID
		}
		fmt.Fprintf(&b, "\n  [%s] %s", c.Status, title)
		if c.Detail != "" {
			fmt.Fprintf(&b, ": %s", c.Detail)
		}
	}
	return b.String()
}

func failedCheckLogLines(data []byte) string {
	var lines []string
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimRight(line, "\r")
		if line == "" {
			continue
		}
		if strings.Contains(line, "[Failed]") || strings.Contains(line, "[Error]") {
			lines = append(lines, line)
		}
	}
	if len(lines) == 0 {
		return ""
	}
	return "failed checks:\n  " + strings.Join(lines, "\n  ")
}

func checkFailureDiagnostics(proc *launchedProc) string {
	if proc == nil || proc.Role != "check" || proc.Session == nil {
		return ""
	}
	// A leftover checks/*.json from a previous attempt is not this launch.
	if proc.InstanceNonce == "" {
		return ""
	}
	if phase := checkPhaseFromArgv(proc.Argv); phase != "" && proc.WorkDir != "" {
		path := filepath.Join(proc.WorkDir, "checks", phase+".json")
		data, err := proc.Session.ReadFile(path)
		if err == nil {
			if diag := formatCheckReport(data); diag != "" {
				return diag
			}
		}
	}
	instanceDir := processInstanceDir(proc)
	data, err := proc.Session.ReadFile(filepath.Join(instanceDir, "stdout.log"))
	if err != nil {
		return ""
	}
	return failedCheckLogLines(data)
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
		LaunchPID:     proc.LaunchPID,
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
					alive, _, err := p.anyAlive()
					if err == nil && !alive {
						_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
							Role: p.Role, Host: p.Host, Instance: p.Instance, PID: p.PID, LaunchPID: p.LaunchPID, InstanceNonce: p.InstanceNonce, State: "failed",
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
					alive, _, err := p.anyAlive()
					if err == nil && !alive {
						_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
							Role: p.Role, Host: p.Host, Instance: p.Instance, PID: p.PID, LaunchPID: p.LaunchPID, InstanceNonce: p.InstanceNonce, State: "failed",
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
					Role: p.Role, Host: p.Host, Instance: p.Instance, PID: p.PID, LaunchPID: p.LaunchPID, InstanceNonce: p.InstanceNonce, State: st,
				})
				_ = o.relayProcessLogs(p, cursors[key])
				progress.Printf("%s/%s finished (%s, exit=%d)", p.Role, p.Instance, st, manifest.ExitStatus)
				p.Finished = true
				if alive, pid, err := p.anyAlive(); err == nil && alive {
					p.warnedAlive = true
					progress.Printf("warning: %s/%s still running after finished (pid %d, exit=%d)", p.Role, p.Instance, pid, manifest.ExitStatus)
				}
				delete(remaining, key)
				if abortOnFail && manifest.ExitStatus != 0 {
					return o.withProcessLogs(p, fmt.Sprintf("%s/%s exited with status %d", p.Role, p.Instance, manifest.ExitStatus))
				}
				continue
			}
			alive, _, err := p.anyAlive()
			if err == nil && !alive {
				// Process died before writing manifest.
				_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
					Role: p.Role, Host: p.Host, Instance: p.Instance, PID: p.PID, LaunchPID: p.LaunchPID, InstanceNonce: p.InstanceNonce, State: "failed",
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
		for _, pid := range uniquePIDs(proc.PID, proc.LaunchPID) {
			if err := sess.Signal(pid, "TERM"); err != nil && first == nil {
				first = err
			}
		}
		_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
			Role: proc.Role, Host: proc.Host, Instance: proc.Instance, PID: proc.PID, LaunchPID: proc.LaunchPID, State: "stopping",
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
		sess, ok := sessions[proc.Host]
		if !ok {
			continue
		}
		for _, pid := range uniquePIDs(proc.PID, proc.LaunchPID) {
			alive, _ := sess.IsAlive(pid)
			if alive {
				_ = sess.Signal(pid, "KILL")
			}
		}
	}
	return first
}

func (o *Orchestrator) finishRemote(ctx *Context, sessions map[string]remote.Session) {
	o.reapLaunched(ctx, sessions)
	closeSessions(sessions)
}

func (o *Orchestrator) reapLaunched(ctx *Context, sessions map[string]remote.Session) {
	if o == nil || len(sessions) == 0 {
		return
	}
	var stopping []*launchedProc
	for _, p := range o.launched {
		if p == nil || p.Session == nil {
			continue
		}
		if _, ok := sessions[p.Host]; !ok {
			continue
		}
		alive, pid, err := p.anyAlive()
		if err != nil || !alive {
			continue
		}
		if p.Finished && !p.warnedAlive {
			p.warnedAlive = true
			progress.Printf("warning: %s/%s still running after finished (pid %d)", p.Role, p.Instance, pid)
		}
		if o.Opts.LeaveProcesses {
			progress.Printf("leaving %s/%s pid %d running (--leave-processes)", p.Role, p.Instance, pid)
			continue
		}
		stopping = append(stopping, p)
	}
	if len(stopping) == 0 {
		return
	}
	for _, p := range stopping {
		pid := p.PID
		if pid <= 0 {
			pid = p.LaunchPID
		}
		progress.Printf("stop %s/%s pid %d (TERM)", p.Role, p.Instance, pid)
		signalPIDs(p.Session, p.pids(), "TERM")
		if ctx != nil && o.StateStore != nil && ctx.RunID != "" {
			_ = o.StateStore.UpsertProcess(ctx.RunID, state.Process{
				Role: p.Role, Host: p.Host, Instance: p.Instance,
				PID: p.PID, LaunchPID: p.LaunchPID, InstanceNonce: p.InstanceNonce,
				State: "stopping",
			})
		}
	}
	grace := 15 * time.Second
	if ctx != nil && ctx.RunConfig != nil && ctx.RunConfig.Phases.StopGraceMs > 0 {
		grace = time.Duration(ctx.RunConfig.Phases.StopGraceMs) * time.Millisecond
	}
	deadline := time.Now().Add(grace)
	for {
		anyAlive := false
		for _, p := range stopping {
			if alive, _, err := p.anyAlive(); err == nil && alive {
				anyAlive = true
				break
			}
		}
		if !anyAlive || time.Now().After(deadline) {
			break
		}
		if err := o.sleep(100 * time.Millisecond); err != nil {
			break
		}
	}
	for _, p := range stopping {
		if alive, pid, err := p.anyAlive(); err == nil && alive {
			progress.Printf("stop %s/%s pid %d (KILL)", p.Role, p.Instance, pid)
			signalPIDs(p.Session, p.pids(), "KILL")
		}
	}
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

// loaderArtifactsOptional reports whether collect may proceed without this
// loader's artifact-manifest. That is normal when load was skipped, or when
// the run only executed later stages (e.g. start --warehouses N against an
// already-loaded database) and never launched this loader instance.
func (o *Orchestrator) loaderArtifactsOptional(ctx *Context, instance string) bool {
	if o == nil || o.StateStore == nil || ctx == nil {
		return false
	}
	rs, err := o.StateStore.Load(ctx.RunID)
	if err != nil {
		return false
	}
	for _, s := range rs.SkippedSteps {
		if s == "load" {
			return true
		}
	}
	for _, p := range rs.Processes {
		if p.Role == "loader" && p.Instance == instance {
			return false
		}
	}
	return true
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
			if role == "loader" && o.loaderArtifactsOptional(ctx, instance) {
				progress.Printf(
					"collect %s/%s: skip (no artifact-manifest; load did not run for this run_id)",
					role, instance,
				)
				return nil
			}
			return fmt.Errorf("missing artifact-manifest for %s/%s at %s", role, instance, manifestRemote)
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

package orchestrator

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"portable-tpcc/mind/internal/collect"
	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/profile"
	"portable-tpcc/mind/internal/progress"
	"portable-tpcc/mind/internal/remote"
	"portable-tpcc/mind/internal/state"
)

type fakeSession struct {
	files      map[string][]byte
	startFiles map[string][]byte
	uploads    []string
	writes     map[string][]byte
	writeModes map[string]os.FileMode
	alive      bool
	downloads  int
	signals    int
	removed    []string
	removedAll []string
	startArgv  []string
	startEnv   map[string]string
}

func (f *fakeSession) Key() string     { return "host-a" }
func (f *fakeSession) Address() string { return "127.0.0.1" }
func (f *fakeSession) Upload(localPath, remotePath string) error {
	f.uploads = append(f.uploads, remotePath)
	return nil
}
func (f *fakeSession) Download(remotePath, localPath string) error {
	f.downloads++
	return nil
}
func (f *fakeSession) ReadFile(remotePath string) ([]byte, error) {
	if data, ok := f.files[remotePath]; ok {
		return data, nil
	}
	for suffix, data := range f.files {
		if strings.HasSuffix(remotePath, suffix) {
			return data, nil
		}
	}
	return nil, fmt.Errorf("missing file %s", remotePath)
}
func (f *fakeSession) WriteFile(remotePath string, data []byte) error {
	return f.WriteFileMode(remotePath, data, 0644)
}
func (f *fakeSession) WriteFileMode(remotePath string, data []byte, mode os.FileMode) error {
	if f.writes == nil {
		f.writes = map[string][]byte{}
	}
	if f.writeModes == nil {
		f.writeModes = map[string]os.FileMode{}
	}
	cp := append([]byte(nil), data...)
	f.writes[remotePath] = cp
	f.writeModes[remotePath] = mode
	if f.files == nil {
		f.files = map[string][]byte{}
	}
	f.files[remotePath] = cp
	return nil
}
func (f *fakeSession) MkdirAll(remotePath string) error {
	return nil
}
func (f *fakeSession) Exists(remotePath string) (bool, error) {
	if _, ok := f.files[remotePath]; ok {
		return true, nil
	}
	for suffix := range f.files {
		if strings.HasSuffix(remotePath, suffix) {
			return true, nil
		}
	}
	return false, nil
}
func (f *fakeSession) Remove(remotePath string) error {
	f.removed = append(f.removed, remotePath)
	delete(f.files, remotePath)
	for suffix := range f.files {
		if suffix != remotePath && strings.HasSuffix(remotePath, suffix) {
			delete(f.files, suffix)
		}
	}
	return nil
}
func (f *fakeSession) RemoveAll(remotePath string) error {
	f.removedAll = append(f.removedAll, remotePath)
	return nil
}
func (f *fakeSession) StartDetached(workDir, binary string, argv []string, env map[string]string, stdoutPath, stderrPath string) (int, error) {
	f.startArgv = append([]string{}, argv...)
	f.startEnv = env
	if len(f.startFiles) > 0 {
		if f.files == nil {
			f.files = map[string][]byte{}
		}
		for k, v := range f.startFiles {
			cp := append([]byte(nil), v...)
			f.files[k] = cp
		}
	}
	return 123, nil
}
func (f *fakeSession) Signal(pid int, sig string) error {
	f.signals++
	f.alive = false
	return nil
}
func (f *fakeSession) IsAlive(pid int) (bool, error) {
	return f.alive, nil
}
func (f *fakeSession) Close() error { return nil }

func writeLaunchRunDir(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "run-config.json"), []byte(`{}`), 0644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestLaunchRoleStoresProcessInstanceNonce(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{
		Profile:    &profile.Profile{},
		Expanded:   config.ExpandedPaths{RemoteRoot: t.TempDir()},
		StateStore: store,
	}
	ctx := &Context{
		RunID:  "run-1",
		RunDir: writeLaunchRunDir(t),
		RunConfig: &config.RunConfig{
			Binary: "tpcc-pgsql",
		},
	}
	process := map[string]interface{}{
		"pid":            456,
		"instance_nonce": "nonce-1",
	}
	data, _ := json.Marshal(process)
	sess := &fakeSession{
		files: map[string][]byte{
			"tpcc-pgsql": []byte("binary"),
		},
		startFiles: map[string][]byte{
			"process.json": data,
		},
		alive: true,
	}

	proc, err := o.launchRole(ctx, map[string]remote.Session{"host-a": sess}, "worker", "host-a", "worker-a", nil)
	if err != nil {
		t.Fatal(err)
	}
	if proc.PID != 456 || proc.LaunchPID != 123 || proc.InstanceNonce != "nonce-1" {
		t.Fatalf("process metadata = pid %d launch_pid %d nonce %q", proc.PID, proc.LaunchPID, proc.InstanceNonce)
	}
	rs, err := store.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	got := rs.Processes["worker/worker-a"]
	if got.InstanceNonce != "nonce-1" || got.PID != 456 {
		t.Fatalf("state process = %+v", got)
	}
}

func TestLaunchRoleFailsWithoutProcessInstanceNonce(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{
		Profile:    &profile.Profile{},
		Expanded:   config.ExpandedPaths{RemoteRoot: t.TempDir()},
		StateStore: store,
	}
	ctx := &Context{
		RunID:  "run-1",
		RunDir: writeLaunchRunDir(t),
		RunConfig: &config.RunConfig{
			Binary: "tpcc-pgsql",
		},
	}
	process := map[string]interface{}{
		"pid": 456,
	}
	data, _ := json.Marshal(process)
	sess := &fakeSession{
		files: map[string][]byte{
			"tpcc-pgsql": []byte("binary"),
		},
		startFiles: map[string][]byte{
			"process.json": data,
		},
		alive: true,
	}

	proc, err := o.launchRole(ctx, map[string]remote.Session{"host-a": sess}, "worker", "host-a", "worker-a", nil)
	if err == nil || !strings.Contains(err.Error(), "missing instance_nonce") {
		t.Fatalf("expected missing nonce error, got proc=%v err=%v", proc, err)
	}
	if sess.signals != 2 {
		t.Fatalf("Signal called %d times, want 2 (launch pid and metadata pid)", sess.signals)
	}
	rs, err := store.Load(ctx.RunID)
	if err != nil {
		t.Fatal(err)
	}
	got := rs.Processes["worker/worker-a"]
	if got.State != "failed" || got.PID != 456 {
		t.Fatalf("state process = %+v", got)
	}
}

func TestWaitProcessMetadataTimesOutWhenMissing(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1"}
	proc := &launchedProc{
		Role:     "worker",
		Host:     "host-a",
		Instance: "worker-a",
		Session: &fakeSession{
			files: map[string][]byte{
				"stderr.log": []byte("nohup: failed to run command 'remote/run/tpcc-x': No such file or directory\n"),
			},
			alive: false,
		},
		PID:      123,
		ProcPath: "/tmp/run/worker/worker-a/process.json",
	}

	err := o.waitProcessMetadata(ctx, proc, 0)
	if err == nil || !strings.Contains(err.Error(), "timeout waiting") {
		t.Fatalf("expected timeout error, got %v", err)
	}
	msg := err.Error()
	if !strings.Contains(msg, "process.json") || !strings.Contains(msg, "alive=false") {
		t.Fatalf("timeout error missing path/alive diagnostics: %v", err)
	}
	if !strings.Contains(msg, "No such file or directory") {
		t.Fatalf("timeout error missing stderr tail: %v", err)
	}
}

func TestWaitProcessMetadataTimeoutSkipsStaleCheckReport(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1"}
	proc := &launchedProc{
		Role:     "check",
		Host:     "host-a",
		Instance: "check-0",
		Session: &fakeSession{
			files: map[string][]byte{
				"checks/after-import.json": []byte(`{
					"phase": "after-import",
					"ok": false,
					"passed": 7,
					"failed": 2,
					"skipped": 20,
					"errors": 0,
					"checks": [
						{"title": "Stock cardinality", "status": "failed", "detail": "stale report"}
					]
				}`),
			},
			alive: true,
		},
		PID:      374563,
		WorkDir:  "/run",
		Argv:     config.CheckArgv("run-config.json", "check-0", "after-import", 0),
		ProcPath: "/run/check/check-0/process.json",
	}

	err := o.waitProcessMetadata(ctx, proc, 0)
	if err == nil || !strings.Contains(err.Error(), "timeout waiting") {
		t.Fatalf("expected timeout error, got %v", err)
	}
	if strings.Contains(err.Error(), "Stock cardinality") || strings.Contains(err.Error(), "stale report") {
		t.Fatalf("stale check report should not be attached before process metadata: %v", err)
	}
}

func TestWaitProcessesReturnsOnInterrupt(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	interrupt, cancel := context.WithCancel(context.Background())
	o := &Orchestrator{
		Opts:       Options{Interrupt: interrupt},
		StateStore: store,
	}
	ctx := &Context{RunID: "run-1"}
	proc := &launchedProc{
		Role:          "loader",
		Host:          "host-a",
		Instance:      "loader-a",
		Session:       &fakeSession{files: map[string][]byte{}, alive: true},
		PID:           123,
		DonePath:      "/done",
		InstanceNonce: "nonce-1",
	}

	errCh := make(chan error, 1)
	go func() {
		errCh <- o.waitProcesses(ctx, []*launchedProc{proc}, time.Minute, true)
	}()
	time.Sleep(20 * time.Millisecond)
	cancel()

	select {
	case err := <-errCh:
		if !errors.Is(err, ErrInterrupted) {
			t.Fatalf("expected ErrInterrupted, got %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("waitProcesses did not return after interrupt")
	}
}

func TestWaitProcessesRejectsStaleManifestNonceWhenProcessDead(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1"}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "worker-a",
		InstanceNonce: "old-nonce",
		Finalized:     true,
		ExitStatus:    0,
	}
	data, _ := json.Marshal(manifest)
	sess := &fakeSession{files: map[string][]byte{"/done": data}, alive: false}
	proc := &launchedProc{
		Role:          "worker",
		Host:          "host-a",
		Instance:      "worker-a",
		Session:       sess,
		PID:           123,
		DonePath:      "/done",
		InstanceNonce: "new-nonce",
	}

	err := o.waitProcesses(ctx, []*launchedProc{proc}, time.Second, true)
	if err == nil || !strings.Contains(err.Error(), "stale manifest nonce") {
		t.Fatalf("expected stale nonce error, got %v", err)
	}
}

func TestWaitProcessesRejectsMissingInstanceNonce(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1"}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "worker-a",
		InstanceNonce: "nonce-1",
		Finalized:     true,
		ExitStatus:    0,
	}
	data, _ := json.Marshal(manifest)
	sess := &fakeSession{files: map[string][]byte{"/done": data}, alive: true}
	proc := &launchedProc{
		Role:     "worker",
		Host:     "host-a",
		Instance: "worker-a",
		Session:  sess,
		PID:      123,
		ProcPath: "/process",
		DonePath: "/done",
	}

	err := o.waitProcesses(ctx, []*launchedProc{proc}, time.Second, true)
	if err == nil || !strings.Contains(err.Error(), "missing instance_nonce") {
		t.Fatalf("expected missing nonce error, got %v", err)
	}
}

func TestLaunchRoleRequiresDeployedBinary(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{
		Profile:    &profile.Profile{},
		Expanded:   config.ExpandedPaths{RemoteRoot: t.TempDir()},
		StateStore: store,
	}
	ctx := &Context{
		RunID: "run-1",
		RunConfig: &config.RunConfig{
			Binary: "tpcc-oceanbase",
		},
	}
	sess := &fakeSession{files: map[string][]byte{}, alive: true}
	_, err := o.launchRole(ctx, map[string]remote.Session{"host-a": sess}, "schema", "host-a", "schema-0", nil)
	if err == nil || !strings.Contains(err.Error(), "not found") || !strings.Contains(err.Error(), "deploy") {
		t.Fatalf("expected missing binary deploy hint, got %v", err)
	}
}

func TestUsesLocalRuntime(t *testing.T) {
	if usesLocalRuntime(&profile.Profile{
		Workers: []profile.NamedHost{{Name: "w", Host: "ob-runner-1"}},
	}) {
		t.Fatal("SSH-only profile should not use local runtime")
	}
	if !usesLocalRuntime(&profile.Profile{
		Loaders: []profile.NamedHost{{Name: "l", Host: "127.0.0.1"}},
		Workers: []profile.NamedHost{{Name: "w", Host: "ob-runner-1"}},
	}) {
		t.Fatal("mixed profile with loopback should use local runtime")
	}
}

func TestCompleteLogLinesAdvancesOnlyPastNewlines(t *testing.T) {
	data := []byte("line1\nline2\npartial")
	lines, off := completeLogLines(data, 0)
	if len(lines) != 2 || lines[0] != "line1" || lines[1] != "line2" {
		t.Fatalf("lines=%v", lines)
	}
	if off != len("line1\nline2\n") {
		t.Fatalf("off=%d", off)
	}
	more := append(data, []byte("ly\nnext\n")...)
	lines, off = completeLogLines(more, off)
	if len(lines) != 2 || lines[0] != "partially" || lines[1] != "next" {
		t.Fatalf("follow-up lines=%v", lines)
	}
	if off != len(more) {
		t.Fatalf("follow-up off=%d want %d", off, len(more))
	}
}

func TestWaitProcessesRelaysRemoteLogs(t *testing.T) {
	var buf bytes.Buffer
	progress.SetWriter(&buf)
	defer progress.SetWriter(nil)

	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1"}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "loader-a",
		InstanceNonce: "nonce-1",
		Finalized:     true,
		ExitStatus:    0,
	}
	data, _ := json.Marshal(manifest)
	sess := &fakeSession{files: map[string][]byte{
		"/done":      data,
		"stderr.log": []byte("Warehouse 1 loaded (1/10)\nWarehouse 2 loaded (2/10)\n"),
	}, alive: false}
	proc := &launchedProc{
		Role:          "loader",
		Host:          "host-a",
		Instance:      "loader-a",
		Session:       sess,
		PID:           123,
		ProcPath:      "/run/loader/loader-a/process.json",
		DonePath:      "/done",
		InstanceNonce: "nonce-1",
	}

	if err := o.waitProcesses(ctx, []*launchedProc{proc}, time.Second, true); err != nil {
		t.Fatal(err)
	}
	out := buf.String()
	if !strings.Contains(out, "[loader/loader-a] Warehouse 1 loaded (1/10)") {
		t.Fatalf("missing relayed log line in progress output: %q", out)
	}
	if !strings.Contains(out, "loader/loader-a finished (exited, exit=0)") {
		t.Fatalf("missing finish progress line: %q", out)
	}
}

func TestWaitProcessesWarnsWhenFinishedButAlive(t *testing.T) {
	var buf bytes.Buffer
	progress.SetWriter(&buf)
	defer progress.SetWriter(nil)

	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1"}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "indexes-0",
		InstanceNonce: "nonce-1",
		Finalized:     true,
		ExitStatus:    0,
	}
	data, _ := json.Marshal(manifest)
	sess := &fakeSession{files: map[string][]byte{"/done": data}, alive: true}
	proc := &launchedProc{
		Role:          "indexes",
		Host:          "host-a",
		Instance:      "indexes-0",
		Session:       sess,
		PID:           373180,
		LaunchPID:     373179,
		DonePath:      "/done",
		InstanceNonce: "nonce-1",
	}

	if err := o.waitProcesses(ctx, []*launchedProc{proc}, time.Second, true); err != nil {
		t.Fatal(err)
	}
	out := buf.String()
	if !strings.Contains(out, "indexes/indexes-0 finished (exited, exit=0)") {
		t.Fatalf("missing finish line: %q", out)
	}
	if !strings.Contains(out, "warning: indexes/indexes-0 still running after finished") {
		t.Fatalf("missing unexpected-alive warning: %q", out)
	}
	if !proc.Finished || !proc.warnedAlive {
		t.Fatalf("Finished=%v warnedAlive=%v", proc.Finished, proc.warnedAlive)
	}
}

func TestReapLaunchedStopsUnexpectedAlive(t *testing.T) {
	var buf bytes.Buffer
	progress.SetWriter(&buf)
	defer progress.SetWriter(nil)

	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1", RunConfig: &config.RunConfig{}}
	sess := &fakeSession{alive: true}
	proc := &launchedProc{
		Role:      "indexes",
		Host:      "host-a",
		Instance:  "indexes-0",
		Session:   sess,
		PID:       373180,
		LaunchPID: 373179,
		Finished:  true,
	}
	o.launched = []*launchedProc{proc}
	o.reapLaunched(ctx, map[string]remote.Session{"host-a": sess})
	if sess.signals < 2 {
		t.Fatalf("Signal called %d times, want launch+binary pids", sess.signals)
	}
	out := buf.String()
	if !strings.Contains(out, "warning: indexes/indexes-0 still running after finished") {
		t.Fatalf("missing warning: %q", out)
	}
	if !strings.Contains(out, "stop indexes/indexes-0") {
		t.Fatalf("missing stop: %q", out)
	}
}

func TestReapLaunchedLeaveProcessesSkipsStop(t *testing.T) {
	var buf bytes.Buffer
	progress.SetWriter(&buf)
	defer progress.SetWriter(nil)

	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{
		StateStore: store,
		Opts:       Options{LeaveProcesses: true},
	}
	ctx := &Context{RunID: "run-1"}
	sess := &fakeSession{alive: true}
	proc := &launchedProc{
		Role:        "indexes",
		Host:        "host-a",
		Instance:    "indexes-0",
		Session:     sess,
		PID:         373180,
		LaunchPID:   373179,
		Finished:    true,
		warnedAlive: true,
	}
	o.launched = []*launchedProc{proc}
	o.reapLaunched(ctx, map[string]remote.Session{"host-a": sess})
	if sess.signals != 0 {
		t.Fatalf("Signal called %d times, want 0 with --leave-processes", sess.signals)
	}
	if !sess.alive {
		t.Fatal("process should still be alive with --leave-processes")
	}
	out := buf.String()
	if !strings.Contains(out, "leaving indexes/indexes-0") || !strings.Contains(out, "--leave-processes") {
		t.Fatalf("missing leave-processes message: %q", out)
	}
}

func TestWaitProcessesFailsUnfinalizedManifestWhenProcessDead(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1"}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "worker-a",
		InstanceNonce: "nonce-1",
		Finalized:     false,
	}
	data, _ := json.Marshal(manifest)
	sess := &fakeSession{files: map[string][]byte{
		"/done":      data,
		"stderr.log": []byte("TPC-C table 'warehouse' already exists. Already inited or forgot to clean?\n"),
	}, alive: false}
	proc := &launchedProc{
		Role:          "worker",
		Host:          "host-a",
		Instance:      "worker-a",
		Session:       sess,
		PID:           123,
		ProcPath:      "/run/worker/worker-a/process.json",
		DonePath:      "/done",
		InstanceNonce: "nonce-1",
	}

	err := o.waitProcesses(ctx, []*launchedProc{proc}, time.Second, true)
	if err == nil || !strings.Contains(err.Error(), "died before finalizing artifacts") {
		t.Fatalf("expected unfinalized dead process error, got %v", err)
	}
	if !strings.Contains(err.Error(), "already exists") {
		t.Fatalf("expected stderr tail in error, got %v", err)
	}
}

func TestCollectArtifactsRejectsTraversalBeforeDownload(t *testing.T) {
	root := t.TempDir()
	o := &Orchestrator{
		Expanded: config.ExpandedPaths{
			RemoteRoot: root + "/remote",
			ResultRoot: root + "/results",
		},
	}
	ctx := &Context{
		RunID: "run-1",
		RunConfig: &config.RunConfig{
			LoadAssignment: []config.LoadAssignmentJSON{
				{Instance: "loader-a", Host: "host-a"},
			},
		},
	}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "loader-a",
		Finalized:     true,
		Payloads: []collect.ArtifactPayloadEntry{
			{Path: "../secret"},
		},
	}
	data, _ := json.Marshal(manifest)
	sess := &fakeSession{files: map[string][]byte{"artifact-manifest.json": data}, alive: true}

	err := o.collectArtifacts(ctx, map[string]remote.Session{"host-a": sess})
	if err == nil || !strings.Contains(err.Error(), "parent traversal") {
		t.Fatalf("expected traversal error, got %v", err)
	}
	if sess.downloads != 0 {
		t.Fatalf("Download called %d times before path validation", sess.downloads)
	}
}

func TestCollectArtifactsRejectsDisallowedPayloadBeforeDownload(t *testing.T) {
	root := t.TempDir()
	o := &Orchestrator{
		Expanded: config.ExpandedPaths{
			RemoteRoot: root + "/remote",
			ResultRoot: root + "/results",
		},
	}
	ctx := &Context{
		RunID: "run-1",
		RunConfig: &config.RunConfig{
			LoadAssignment: []config.LoadAssignmentJSON{
				{Instance: "loader-a", Host: "host-a"},
			},
		},
	}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "loader-a",
		Finalized:     true,
		Payloads: []collect.ArtifactPayloadEntry{
			{Path: "logs/stdout.log"},
		},
	}
	data, _ := json.Marshal(manifest)
	sess := &fakeSession{files: map[string][]byte{"artifact-manifest.json": data}, alive: true}

	err := o.collectArtifacts(ctx, map[string]remote.Session{"host-a": sess})
	if err == nil || !strings.Contains(err.Error(), "unsupported artifact payload") {
		t.Fatalf("expected disallowed payload error, got %v", err)
	}
	if sess.downloads != 0 {
		t.Fatalf("Download called %d times before path validation", sess.downloads)
	}
}

func TestCollectArtifactsRejectsLocalSymlinkPayloadEscape(t *testing.T) {
	root := t.TempDir()
	remoteRoot := filepath.Join(root, "remote")
	instanceDir := filepath.Join(remoteRoot, "run-1", "loader", "loader-a")
	if err := os.MkdirAll(instanceDir, 0755); err != nil {
		t.Fatal(err)
	}
	outside := filepath.Join(root, "secret")
	if err := os.WriteFile(outside, []byte("secret"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(instanceDir, "result.json")); err != nil {
		t.Fatal(err)
	}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "loader-a",
		Finalized:     true,
		Payloads: []collect.ArtifactPayloadEntry{
			{Path: "result.json"},
		},
	}
	data, _ := json.Marshal(manifest)
	if err := os.WriteFile(filepath.Join(instanceDir, "artifact-manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
	}
	sess, err := remote.NewLocal("host-a", "127.0.0.1", remoteRoot)
	if err != nil {
		t.Fatal(err)
	}
	o := &Orchestrator{
		Expanded: config.ExpandedPaths{
			RemoteRoot: remoteRoot,
			ResultRoot: filepath.Join(root, "results"),
		},
	}
	ctx := &Context{
		RunID: "run-1",
		RunConfig: &config.RunConfig{
			LoadAssignment: []config.LoadAssignmentJSON{
				{Instance: "loader-a", Host: "host-a"},
			},
		},
	}

	err = o.collectArtifacts(ctx, map[string]remote.Session{"host-a": sess})
	if err == nil || !strings.Contains(err.Error(), "escapes base") {
		t.Fatalf("expected symlink escape error, got %v", err)
	}
}

func TestCollectArtifactsWritesRawInstanceDir(t *testing.T) {
	root := t.TempDir()
	remoteRoot := filepath.Join(root, "remote")
	instanceDir := filepath.Join(remoteRoot, "run-1", "loader", "loader-a")
	if err := os.MkdirAll(instanceDir, 0755); err != nil {
		t.Fatal(err)
	}
	payload := []byte(`{"ok":true}`)
	if err := os.WriteFile(filepath.Join(instanceDir, "result.json"), payload, 0644); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(payload)
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "loader-a",
		Finalized:     true,
		Payloads: []collect.ArtifactPayloadEntry{
			{Path: "result.json", Size: int64(len(payload)), SHA256: hex.EncodeToString(sum[:])},
		},
	}
	data, _ := json.Marshal(manifest)
	if err := os.WriteFile(filepath.Join(instanceDir, "artifact-manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
	}
	sess, err := remote.NewLocal("host-a", "127.0.0.1", remoteRoot)
	if err != nil {
		t.Fatal(err)
	}
	o := &Orchestrator{
		Expanded: config.ExpandedPaths{
			RemoteRoot: remoteRoot,
			ResultRoot: filepath.Join(root, "results"),
		},
	}
	ctx := &Context{
		RunID: "run-1",
		RunConfig: &config.RunConfig{
			LoadAssignment: []config.LoadAssignmentJSON{
				{Instance: "loader-a", Host: "host-a"},
			},
		},
	}

	if err := o.collectArtifacts(ctx, map[string]remote.Session{"host-a": sess}); err != nil {
		t.Fatal(err)
	}
	dest := filepath.Join(root, "results", "run-1", "raw", "loader", "loader-a", "result.json")
	got, err := os.ReadFile(dest)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(payload) {
		t.Fatalf("collected payload=%q, want %q", got, payload)
	}
}

func TestCollectArtifactsSkipsMissingLoaderWhenLoadDidNotRun(t *testing.T) {
	root := t.TempDir()
	store := &state.Store{StateDir: filepath.Join(root, "state")}
	o := &Orchestrator{
		Expanded: config.ExpandedPaths{
			RemoteRoot: filepath.Join(root, "remote"),
			ResultRoot: filepath.Join(root, "results"),
		},
		StateStore: store,
	}
	ctx := &Context{
		RunID: "run-1",
		RunConfig: &config.RunConfig{
			LoadAssignment: []config.LoadAssignmentJSON{
				{Instance: "loader-a", Host: "host-a"},
			},
		},
	}
	if err := store.Save(&state.RunState{
		SchemaVersion: 1,
		RunID:         ctx.RunID,
		State:         state.StateCollecting,
		// start-only run: workers may exist, but no loader was launched.
		Processes: map[string]state.Process{
			"worker/worker-a": {
				Role: "worker", Host: "host-a", Instance: "worker-a", State: "exited",
			},
		},
	}); err != nil {
		t.Fatal(err)
	}
	sess := &fakeSession{files: map[string][]byte{}, alive: true}

	if err := o.collectArtifacts(ctx, map[string]remote.Session{"host-a": sess}); err != nil {
		t.Fatalf("expected soft-skip of missing loader, got %v", err)
	}
}

func TestCollectArtifactsRequiresLoaderWhenLoadLaunched(t *testing.T) {
	root := t.TempDir()
	store := &state.Store{StateDir: filepath.Join(root, "state")}
	o := &Orchestrator{
		Expanded: config.ExpandedPaths{
			RemoteRoot: filepath.Join(root, "remote"),
			ResultRoot: filepath.Join(root, "results"),
		},
		StateStore: store,
	}
	ctx := &Context{
		RunID: "run-1",
		RunConfig: &config.RunConfig{
			LoadAssignment: []config.LoadAssignmentJSON{
				{Instance: "loader-a", Host: "host-a"},
			},
		},
	}
	if err := store.Save(&state.RunState{
		SchemaVersion: 1,
		RunID:         ctx.RunID,
		State:         state.StateCollecting,
		Processes: map[string]state.Process{
			"loader/loader-a": {
				Role: "loader", Host: "host-a", Instance: "loader-a", State: "exited",
			},
		},
	}); err != nil {
		t.Fatal(err)
	}
	sess := &fakeSession{files: map[string][]byte{}, alive: true}

	err := o.collectArtifacts(ctx, map[string]remote.Session{"host-a": sess})
	if err == nil || !strings.Contains(err.Error(), "missing artifact-manifest for loader/loader-a") {
		t.Fatalf("expected hard error for launched loader, got %v", err)
	}
}

func TestCredentialFilesForRun(t *testing.T) {
	root := t.TempDir()
	caPath := filepath.Join(root, "root.pem")
	saPath := filepath.Join(root, "sa.json")
	if err := os.WriteFile(caPath, []byte("CA"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(saPath, []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}
	o := &Orchestrator{
		Profile: &profile.Profile{
			Database: profile.Database{
				DBMS:      "ydb",
				CaFile:    caPath,
				SaKeyFile: saPath,
			},
		},
	}
	files, err := o.credentialFilesForRun()
	if err != nil {
		t.Fatal(err)
	}
	if len(files) != 2 {
		t.Fatalf("got %d files", len(files))
	}
	byName := map[string]string{}
	for _, f := range files {
		byName[f.RemoteName] = f.LocalPath
	}
	if byName[config.RemoteCAFileName] != caPath {
		t.Fatalf("ca local=%q", byName[config.RemoteCAFileName])
	}
	if byName[config.RemoteSAKeyFileName] != saPath {
		t.Fatalf("sa local=%q", byName[config.RemoteSAKeyFileName])
	}
}

func TestDeployToHostsUploadsOnlyBinary(t *testing.T) {
	root := t.TempDir()
	caPath := filepath.Join(root, "root.pem")
	saPath := filepath.Join(root, "sa.json")
	binPath := filepath.Join(root, "tpcc-ydb")
	runDir := filepath.Join(root, "run")
	if err := os.MkdirAll(runDir, 0755); err != nil {
		t.Fatal(err)
	}
	for _, p := range []struct {
		path string
		data string
	}{
		{caPath, "CA"},
		{saPath, "{}"},
		{binPath, "bin"},
		{filepath.Join(runDir, "run-config.json"), "{}"},
	} {
		if err := os.WriteFile(p.path, []byte(p.data), 0644); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("TPCC_PASSWORD", "s3cret")
	o := &Orchestrator{
		Profile: &profile.Profile{
			Database: profile.Database{
				DBMS:        "ydb",
				CaFile:      caPath,
				SaKeyFile:   saPath,
				PasswordEnv: "TPCC_PASSWORD",
			},
		},
		Expanded: config.ExpandedPaths{RemoteRoot: filepath.Join(root, "remote")},
		Opts:     Options{WorkerBinary: binPath},
	}
	sess := &fakeSession{}
	if err := o.deployToHosts(map[string]remote.Session{"host-a": sess}); err != nil {
		t.Fatal(err)
	}
	if len(sess.uploads) != 1 || !strings.HasSuffix(sess.uploads[0], "tpcc-ydb") {
		t.Fatalf("deploy uploads=%v, want only shared binary", sess.uploads)
	}
	if strings.Contains(sess.uploads[0], "run-1") {
		t.Fatalf("binary should be shared under remote_root, got %q", sess.uploads[0])
	}
	for _, u := range sess.uploads {
		if strings.Contains(u, "run-config.json") || strings.Contains(u, config.RemoteCAFileName) {
			t.Fatalf("deploy must not upload run-scoped files: %v", sess.uploads)
		}
	}
	if len(sess.writes) != 0 {
		t.Fatalf("deploy must not write password file: %v", sess.writes)
	}
	if len(sess.removed) != 0 {
		t.Fatalf("first deploy should not remove a missing binary: %v", sess.removed)
	}
}

func TestDeployToHostsRemovesExistingBinary(t *testing.T) {
	root := t.TempDir()
	binPath := filepath.Join(root, "tpcc-ydb")
	if err := os.WriteFile(binPath, []byte("bin"), 0755); err != nil {
		t.Fatal(err)
	}
	remoteRoot := filepath.Join(root, "remote")
	o := &Orchestrator{
		Profile:  &profile.Profile{Database: profile.Database{DBMS: "ydb"}},
		Expanded: config.ExpandedPaths{RemoteRoot: remoteRoot},
		Opts:     Options{WorkerBinary: binPath},
	}
	remoteBin := filepath.Join(remoteRoot, "tpcc-ydb")
	sess := &fakeSession{files: map[string][]byte{remoteBin: []byte("old")}}
	if err := o.deployToHosts(map[string]remote.Session{"host-a": sess}); err != nil {
		t.Fatal(err)
	}
	if len(sess.removed) != 1 || sess.removed[0] != remoteBin {
		t.Fatalf("removed=%v, want [%q]", sess.removed, remoteBin)
	}
	if len(sess.uploads) != 1 || sess.uploads[0] != remoteBin {
		t.Fatalf("uploads=%v, want [%q]", sess.uploads, remoteBin)
	}
}

func TestLaunchRoleUploadsRunConfigAndCredentials(t *testing.T) {
	root := t.TempDir()
	caPath := filepath.Join(root, "root.pem")
	saPath := filepath.Join(root, "sa.json")
	if err := os.WriteFile(caPath, []byte("CA"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(saPath, []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}
	store := &state.Store{StateDir: t.TempDir()}
	remoteRoot := filepath.Join(root, "remote")
	o := &Orchestrator{
		Profile: &profile.Profile{
			Database: profile.Database{
				DBMS:      "ydb",
				CaFile:    caPath,
				SaKeyFile: saPath,
			},
		},
		Expanded:   config.ExpandedPaths{RemoteRoot: remoteRoot},
		StateStore: store,
	}
	ctx := &Context{
		RunID:  "run-1",
		RunDir: writeLaunchRunDir(t),
		RunConfig: &config.RunConfig{
			Binary: "tpcc-ydb",
		},
	}
	process := map[string]interface{}{
		"pid":            456,
		"instance_nonce": "nonce-1",
	}
	data, _ := json.Marshal(process)
	sess := &fakeSession{
		files: map[string][]byte{
			"tpcc-ydb": []byte("binary"),
		},
		startFiles: map[string][]byte{
			"process.json": data,
		},
		alive: true,
	}

	if _, err := o.launchRole(ctx, map[string]remote.Session{"host-a": sess}, "loader", "host-a", "loader-a", []string{"loader"}); err != nil {
		t.Fatal(err)
	}
	joined := strings.Join(sess.uploads, "\n")
	for _, want := range []string{"run-config.json", config.RemoteCAFileName, config.RemoteSAKeyFileName} {
		if !strings.Contains(joined, want) {
			t.Fatalf("missing launch upload %q in %v", want, sess.uploads)
		}
	}
	for _, u := range sess.uploads {
		if strings.HasSuffix(u, "run-config.json") && !strings.Contains(u, "run-1") {
			t.Fatalf("run-config should land under run dir, got %q", u)
		}
	}
}

func TestLaunchRoleDoesNotPassPasswordEnv(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	t.Setenv("TPCC_PASSWORD", "s3cret")
	o := &Orchestrator{
		Profile: &profile.Profile{
			Database: profile.Database{
				DBMS:        "pgsql",
				PasswordEnv: "TPCC_PASSWORD",
			},
		},
		Expanded:   config.ExpandedPaths{RemoteRoot: t.TempDir()},
		StateStore: store,
	}
	ctx := &Context{
		RunID:  "run-1",
		RunDir: writeLaunchRunDir(t),
		RunConfig: &config.RunConfig{
			Binary: "tpcc-pgsql",
		},
	}
	process := map[string]interface{}{
		"pid":            456,
		"instance_nonce": "nonce-1",
	}
	data, _ := json.Marshal(process)
	sess := &fakeSession{
		files: map[string][]byte{
			"tpcc-pgsql": []byte("binary"),
		},
		startFiles: map[string][]byte{
			"process.json": data,
		},
		alive: true,
	}

	if _, err := o.launchRole(ctx, map[string]remote.Session{"host-a": sess}, "loader", "host-a", "loader-a", []string{"loader"}); err != nil {
		t.Fatal(err)
	}
	if sess.startEnv != nil {
		t.Fatalf("StartDetached env=%v, want nil (password via file)", sess.startEnv)
	}
	found := false
	for path, data := range sess.writes {
		if strings.HasSuffix(path, config.RemotePasswordFileName) && string(data) == "s3cret" {
			found = true
			if sess.writeModes[path] != 0600 {
				t.Fatalf("mode=%v", sess.writeModes[path])
			}
		}
	}
	if !found {
		t.Fatalf("password file not written: %v", sess.writes)
	}
	cfgUploaded := false
	for _, u := range sess.uploads {
		if strings.HasSuffix(u, "run-config.json") {
			cfgUploaded = true
		}
	}
	if !cfgUploaded {
		t.Fatalf("run-config not uploaded at launch: %v", sess.uploads)
	}
}

func TestLaunchRoleClearsStaleInstanceMetadata(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{
		Profile:    &profile.Profile{},
		Expanded:   config.ExpandedPaths{RemoteRoot: t.TempDir()},
		StateStore: store,
	}
	ctx := &Context{
		RunID:  "run-1",
		RunDir: writeLaunchRunDir(t),
		RunConfig: &config.RunConfig{
			Binary: "tpcc-pgsql",
		},
	}
	oldProcess, _ := json.Marshal(map[string]interface{}{
		"pid":            111,
		"instance_nonce": "old-nonce",
	})
	newProcess, _ := json.Marshal(map[string]interface{}{
		"pid":            456,
		"instance_nonce": "new-nonce",
	})
	oldManifest, _ := json.Marshal(collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "check-0",
		InstanceNonce: "old-nonce",
		Finalized:     true,
		ExitStatus:    1,
	})
	sess := &fakeSession{
		files: map[string][]byte{
			"tpcc-pgsql":             []byte("binary"),
			"process.json":           oldProcess,
			"artifact-manifest.json": oldManifest,
		},
		startFiles: map[string][]byte{
			"process.json": newProcess,
		},
		alive: true,
	}

	proc, err := o.launchRole(ctx, map[string]remote.Session{"host-a": sess}, "check", "host-a", "check-0", config.CheckArgv("run-config.json", "check-0", "after-import", 0))
	if err != nil {
		t.Fatal(err)
	}
	if proc.InstanceNonce != "new-nonce" {
		t.Fatalf("nonce=%q, want new-nonce (stale process.json must not be reused)", proc.InstanceNonce)
	}
	removed := strings.Join(sess.removed, "\n")
	if !strings.Contains(removed, "process.json") || !strings.Contains(removed, "artifact-manifest.json") {
		t.Fatalf("stale metadata not removed: %v", sess.removed)
	}
	if _, ok := sess.files["artifact-manifest.json"]; ok {
		t.Fatal("stale artifact-manifest.json still present after launch")
	}
}

func TestWaitProcessesIncludesCheckReportDiagnostics(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1"}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "check-0",
		InstanceNonce: "nonce-1",
		Finalized:     true,
		ExitStatus:    1,
	}
	data, _ := json.Marshal(manifest)
	report := []byte(`{
		"schema_version": 1,
		"phase": "after-import",
		"passed": 7,
		"failed": 2,
		"skipped": 20,
		"errors": 0,
		"ok": false,
		"checks": [
			{"id": "stock_count", "title": "Stock cardinality", "status": "failed", "detail": "Query failed: [4012] Timeout"},
			{"id": "order_line_count", "title": "Order-line cardinality (post-import)", "status": "failed", "detail": "Query failed: [4012] Timeout"},
			{"id": "w_ytd", "title": "W_YTD equals sum(D_YTD)", "status": "skipped", "detail": "skipped: base cardinality failed"}
		]
	}`)
	sess := &fakeSession{files: map[string][]byte{
		"/done":                    data,
		"checks/after-import.json": report,
		"stderr.log":               []byte("INFO: Check report written to ./checks/after-import.json\n"),
		"stdout.log":               []byte("Checking W_YTD equals sum(D_YTD) [Skipped]: skipped: base cardinality failed\n"),
	}, alive: false}
	proc := &launchedProc{
		Role:          "check",
		Host:          "host-a",
		Instance:      "check-0",
		Session:       sess,
		PID:           123,
		WorkDir:       "/run",
		ProcPath:      "/run/check/check-0/process.json",
		DonePath:      "/done",
		Argv:          config.CheckArgv("run-config.json", "check-0", "after-import", 0),
		InstanceNonce: "nonce-1",
	}

	err := o.waitProcesses(ctx, []*launchedProc{proc}, time.Second, true)
	if err == nil {
		t.Fatal("expected check failure")
	}
	msg := err.Error()
	if !strings.Contains(msg, "check/check-0 exited with status 1") {
		t.Fatalf("missing exit status: %v", err)
	}
	if !strings.Contains(msg, "check after-import: 7 passed, 2 failed, 20 skipped, 0 errors") {
		t.Fatalf("missing check summary: %v", err)
	}
	if !strings.Contains(msg, "[failed] Stock cardinality: Query failed: [4012] Timeout") {
		t.Fatalf("missing stock failure: %v", err)
	}
	if !strings.Contains(msg, "[failed] Order-line cardinality (post-import)") {
		t.Fatalf("missing order-line failure: %v", err)
	}
	if strings.Contains(msg, "W_YTD equals sum(D_YTD)") {
		t.Fatalf("skipped checks should not dominate diagnostics: %v", err)
	}
	if !strings.Contains(msg, "Check report written") {
		t.Fatalf("missing stderr tail: %v", err)
	}
}

func TestWaitProcessesFallsBackToFailedStdoutLines(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{StateStore: store}
	ctx := &Context{RunID: "run-1"}
	manifest := collect.ArtifactManifest{
		SchemaVersion: 1,
		Instance:      "check-0",
		InstanceNonce: "nonce-1",
		Finalized:     true,
		ExitStatus:    1,
	}
	data, _ := json.Marshal(manifest)
	sess := &fakeSession{files: map[string][]byte{
		"/done": data,
		"stdout.log": []byte("Checking Warehouse cardinality [OK]\n" +
			"Checking Stock cardinality [Failed]: Query failed: [4012] Timeout\n" +
			"Checking History cardinality (post-import) [OK]\n"),
	}, alive: false}
	proc := &launchedProc{
		Role:          "check",
		Host:          "host-a",
		Instance:      "check-0",
		Session:       sess,
		PID:           123,
		WorkDir:       "/run",
		ProcPath:      "/run/check/check-0/process.json",
		DonePath:      "/done",
		Argv:          config.CheckArgv("run-config.json", "check-0", "after-import", 0),
		InstanceNonce: "nonce-1",
	}

	err := o.waitProcesses(ctx, []*launchedProc{proc}, time.Second, true)
	if err == nil || !strings.Contains(err.Error(), "Checking Stock cardinality [Failed]: Query failed: [4012] Timeout") {
		t.Fatalf("expected failed stdout line in error, got %v", err)
	}
}

func TestFormatCheckReport(t *testing.T) {
	got := formatCheckReport([]byte(`{"ok":true,"passed":3,"failed":0,"skipped":0,"errors":0,"phase":"after-run"}`))
	if got != "" {
		t.Fatalf("ok report should be empty, got %q", got)
	}
	got = formatCheckReport([]byte(`not json`))
	if got != "" {
		t.Fatalf("invalid JSON should be empty, got %q", got)
	}
}

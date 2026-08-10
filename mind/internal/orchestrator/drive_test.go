package orchestrator

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"portable-tpcc/mind/internal/collect"
	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/profile"
	"portable-tpcc/mind/internal/remote"
	"portable-tpcc/mind/internal/state"
)

type fakeSession struct {
	files     map[string][]byte
	uploads   []string
	alive     bool
	downloads int
	signals   int
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
	return nil
}
func (f *fakeSession) StartDetached(workDir, binary string, argv []string, env map[string]string, stdoutPath, stderrPath string) (int, error) {
	return 123, nil
}
func (f *fakeSession) Signal(pid int, sig string) error {
	f.signals++
	return nil
}
func (f *fakeSession) IsAlive(pid int) (bool, error) {
	return f.alive, nil
}
func (f *fakeSession) Close() error { return nil }

func TestLaunchRoleStoresProcessInstanceNonce(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	o := &Orchestrator{
		Profile:    &profile.Profile{},
		Expanded:   config.ExpandedPaths{RemoteRoot: t.TempDir()},
		StateStore: store,
	}
	ctx := &Context{
		RunID: "run-1",
		RunConfig: &config.RunConfig{
			Binary: "tpcc-pgsql",
		},
	}
	process := map[string]interface{}{
		"pid":            456,
		"instance_nonce": "nonce-1",
	}
	data, _ := json.Marshal(process)
	sess := &fakeSession{files: map[string][]byte{"process.json": data}, alive: true}

	proc, err := o.launchRole(ctx, map[string]remote.Session{"host-a": sess}, "worker", "host-a", "worker-a", nil)
	if err != nil {
		t.Fatal(err)
	}
	if proc.PID != 456 || proc.InstanceNonce != "nonce-1" {
		t.Fatalf("process metadata = pid %d nonce %q", proc.PID, proc.InstanceNonce)
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
		RunID: "run-1",
		RunConfig: &config.RunConfig{
			Binary: "tpcc-pgsql",
		},
	}
	process := map[string]interface{}{
		"pid": 456,
	}
	data, _ := json.Marshal(process)
	sess := &fakeSession{files: map[string][]byte{"process.json": data}, alive: true}

	proc, err := o.launchRole(ctx, map[string]remote.Session{"host-a": sess}, "worker", "host-a", "worker-a", nil)
	if err == nil || !strings.Contains(err.Error(), "missing instance_nonce") {
		t.Fatalf("expected missing nonce error, got proc=%v err=%v", proc, err)
	}
	if sess.signals != 1 {
		t.Fatalf("Signal called %d times, want 1", sess.signals)
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
		Session:  &fakeSession{files: map[string][]byte{}, alive: true},
		PID:      123,
		ProcPath: "/missing",
	}

	err := o.waitProcessMetadata(ctx, proc, 0)
	if err == nil || !strings.Contains(err.Error(), "timeout waiting") {
		t.Fatalf("expected timeout error, got %v", err)
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
	sess := &fakeSession{files: map[string][]byte{"/done": data}, alive: false}
	proc := &launchedProc{
		Role:          "worker",
		Host:          "host-a",
		Instance:      "worker-a",
		Session:       sess,
		PID:           123,
		DonePath:      "/done",
		InstanceNonce: "nonce-1",
	}

	err := o.waitProcesses(ctx, []*launchedProc{proc}, time.Second, true)
	if err == nil || !strings.Contains(err.Error(), "died before finalizing artifacts") {
		t.Fatalf("expected unfinalized dead process error, got %v", err)
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

func TestCredentialFilesForDeploy(t *testing.T) {
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
	files, err := o.credentialFilesForDeploy()
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

func TestDeployToHostsUploadsCredentialFiles(t *testing.T) {
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
	o := &Orchestrator{
		Profile: &profile.Profile{
			Database: profile.Database{
				DBMS:      "ydb",
				CaFile:    caPath,
				SaKeyFile: saPath,
			},
		},
		Expanded: config.ExpandedPaths{RemoteRoot: filepath.Join(root, "remote")},
		Opts:     Options{WorkerBinary: binPath},
	}
	ctx := &Context{RunID: "run-1", RunDir: runDir}
	sess := &fakeSession{}
	if err := o.deployToHosts(ctx, map[string]remote.Session{"host-a": sess}); err != nil {
		t.Fatal(err)
	}
	joined := strings.Join(sess.uploads, "\n")
	for _, want := range []string{"tpcc-ydb", "run-config.json", config.RemoteCAFileName, config.RemoteSAKeyFileName} {
		if !strings.Contains(joined, want) {
			t.Fatalf("missing upload %q in %v", want, sess.uploads)
		}
	}
}

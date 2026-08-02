package orchestrator

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"portable-tpcc/tpccctl/internal/collect"
	"portable-tpcc/tpccctl/internal/config"
	"portable-tpcc/tpccctl/internal/profile"
	"portable-tpcc/tpccctl/internal/remote"
	"portable-tpcc/tpccctl/internal/state"
)

type fakeSession struct {
	files     map[string][]byte
	alive     bool
	downloads int
}

func (f *fakeSession) Key() string     { return "host-a" }
func (f *fakeSession) Address() string { return "127.0.0.1" }
func (f *fakeSession) Upload(localPath, remotePath string) error {
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

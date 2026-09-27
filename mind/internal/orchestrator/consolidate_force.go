package orchestrator

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"portable-tpcc/mind/internal/collect"
	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/profile"
	"portable-tpcc/mind/internal/state"
)

// consolidateIdentityError is a profile-identity mismatch. Run scanners skip
// these. I/O failures are returned as ordinary errors.
type consolidateIdentityError struct {
	msg string
}

func (e *consolidateIdentityError) Error() string { return e.msg }

func identityErr(format string, args ...interface{}) error {
	return &consolidateIdentityError{msg: fmt.Sprintf(format, args...)}
}

func rejectForceConsolidateOverrides(o config.ProfileOverrides) error {
	var flags []string
	if o.Warehouses != nil {
		flags = append(flags, "--warehouses")
	}
	if o.RampUp != nil {
		flags = append(flags, "--ramp-up")
	}
	if o.Measurement != nil {
		flags = append(flags, "--measurement")
	}
	if len(flags) == 0 {
		return nil
	}
	return fmt.Errorf("consolidate --force does not accept %s", strings.Join(flags, ", "))
}

// LoadConsolidateContext reads an existing run for consolidate --force.
// It does not compare profile.sha256, does not build a new run-config, and
// does not rewrite run-config.json, profile.sha256, or profile.redacted.yaml.
func (o *Orchestrator) LoadConsolidateContext(runID string) (*Context, error) {
	if err := o.verifyConsolidateIdentity(runID); err != nil {
		return nil, err
	}
	rc, err := o.readRunConfig(runID)
	if err != nil {
		return nil, err
	}
	rc.RunID = runID
	return &Context{
		RunID:     runID,
		RunConfig: rc,
		RunDir:    o.StateStore.RunDir(runID),
	}, nil
}

// latestForceConsolidateRunID returns the newest run under state_dir whose
// recorded identity matches the live profile, when that run is non-terminal.
// A newer terminal run of the same identity hides older runs, matching the
// default continuable-run rule. Byte equality of the profile file is not required.
func (o *Orchestrator) latestForceConsolidateRunID() (string, error) {
	root := filepath.Join(o.StateStore.StateDir, "runs")
	entries, err := os.ReadDir(root)
	if err != nil {
		if os.IsNotExist(err) {
			return "", nil
		}
		return "", err
	}
	var bestID string
	var bestTime time.Time
	bestTerminal := false
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		runID := e.Name()
		match, err := o.forceIdentityMatch(runID)
		if err != nil {
			return "", err
		}
		if !match {
			continue
		}
		rs, err := o.StateStore.Load(runID)
		if err != nil {
			return "", err
		}
		t, err := time.Parse(time.RFC3339, rs.UpdatedAt)
		if err != nil {
			info, ierr := e.Info()
			if ierr != nil {
				continue
			}
			t = info.ModTime()
		}
		if bestID != "" && !t.After(bestTime) {
			continue
		}
		bestID = runID
		bestTime = t
		bestTerminal = state.IsTerminal(rs.State)
	}
	if bestID == "" || bestTerminal {
		return "", nil
	}
	return bestID, nil
}

func (o *Orchestrator) forceIdentityMatch(runID string) (bool, error) {
	err := o.verifyConsolidateIdentity(runID)
	if err == nil {
		return true, nil
	}
	if _, ok := err.(*consolidateIdentityError); ok {
		return false, nil
	}
	return false, err
}

// verifyConsolidateIdentity checks that the live profile still names the
// recorded run. Scale, workload, phases, runtime, and checks are not compared.
// paths.state_dir and paths.result_root always are. Loader hosts, remote_root,
// and SSH dial settings are compared only when collect still has to run.
func (o *Orchestrator) verifyConsolidateIdentity(runID string) error {
	rc, err := o.readRunConfig(runID)
	if err != nil {
		return err
	}
	snap, err := o.readRecordedProfile(runID)
	if err != nil {
		return err
	}
	p := o.Profile
	if p.Metadata.Name != rc.ProfileName {
		return identityErr(
			"consolidate --force: profile name %q does not match run %s profile_name %q",
			p.Metadata.Name, runID, rc.ProfileName,
		)
	}
	if p.Database.DBMS != rc.Database.DBMS {
		return identityErr(
			"consolidate --force: database.dbms %q does not match run %s (%q)",
			p.Database.DBMS, runID, rc.Database.DBMS,
		)
	}
	if !sameHostSet(namedHosts(p.Workers), assignmentHosts(rc.WorkerAssignment)) {
		return identityErr(
			"consolidate --force: workers hosts do not match run %s worker_assignment",
			runID,
		)
	}
	if err := authMatchesRecorded(p, snap, rc, runID); err != nil {
		return err
	}
	snapExp, err := config.ExpandProfilePaths(snap)
	if err != nil {
		return fmt.Errorf("consolidate --force: recorded profile paths for run %s: %w", runID, err)
	}
	if snapExp.StateDir != o.Expanded.StateDir {
		return identityErr(
			"consolidate --force: paths.state_dir does not match the recorded profile for run %s",
			runID,
		)
	}
	if snapExp.ResultRoot != o.Expanded.ResultRoot {
		return identityErr(
			"consolidate --force: paths.result_root does not match the recorded profile for run %s",
			runID,
		)
	}
	if collect.HasCollectionManifest(o.Expanded.ResultRoot, runID) {
		return nil
	}
	if !sameHostSet(namedHosts(p.Loaders), loadAssignmentHosts(rc.LoadAssignment)) {
		return identityErr(
			"consolidate --force: loaders hosts do not match run %s load_assignment",
			runID,
		)
	}
	if p.Paths.RemoteRoot != snap.Paths.RemoteRoot {
		return identityErr(
			"consolidate --force: paths.remote_root does not match the recorded profile for run %s",
			runID,
		)
	}
	return sshMatchesRecorded(p, snap, o.Expanded.KnownHosts, snapExp.KnownHosts, runID)
}

func (o *Orchestrator) readRunConfig(runID string) (*config.RunConfig, error) {
	path := filepath.Join(o.StateStore.RunDir(runID), "run-config.json")
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, identityErr("consolidate --force: run %s is missing run-config.json", runID)
		}
		return nil, err
	}
	rc := &config.RunConfig{}
	if err := json.Unmarshal(data, rc); err != nil {
		return nil, identityErr("consolidate --force: run %s run-config.json: %v", runID, err)
	}
	if rc.RunID != "" && rc.RunID != runID {
		return nil, identityErr("consolidate --force: run-config run_id %q does not match %q", rc.RunID, runID)
	}
	return rc, nil
}

func (o *Orchestrator) readRecordedProfile(runID string) (*profile.Profile, error) {
	path := filepath.Join(o.StateStore.RunDir(runID), "profile.redacted.yaml")
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, identityErr("consolidate --force: run %s is missing profile.redacted.yaml", runID)
		}
		return nil, err
	}
	snap, err := profile.Parse(data)
	if err != nil {
		return nil, identityErr("consolidate --force: run %s profile.redacted.yaml: %v", runID, err)
	}
	return snap, nil
}

func authMatchesRecorded(live, snap *profile.Profile, rc *config.RunConfig, runID string) error {
	db := live.Database
	if effectiveAuthScheme(db) != rc.Database.AuthScheme {
		return identityErr(
			"consolidate --force: database.auth_scheme does not match run %s",
			runID,
		)
	}
	if db.User != rc.Database.User {
		return identityErr("consolidate --force: database.user does not match run %s", runID)
	}
	wantPassword := ""
	if config.NeedsRemotePasswordFile(db) {
		wantPassword = config.RemotePasswordFileName
	}
	if rc.Database.PasswordFile != wantPassword {
		return identityErr(
			"consolidate --force: database authentication does not match run %s",
			runID,
		)
	}
	if (db.CaFile != "") != (rc.Database.CaFile != "") {
		return identityErr("consolidate --force: database.ca_file does not match run %s", runID)
	}
	if (db.SaKeyFile != "") != (rc.Database.SaKeyFile != "") {
		return identityErr("consolidate --force: database.sa_key_file does not match run %s", runID)
	}
	recorded := snap.Database
	if db.PasswordEnv != recorded.PasswordEnv {
		return identityErr(
			"consolidate --force: database.password_env does not match the recorded profile for run %s",
			runID,
		)
	}
	if db.CaFile != recorded.CaFile {
		return identityErr(
			"consolidate --force: database.ca_file does not match the recorded profile for run %s",
			runID,
		)
	}
	if db.SaKeyFile != recorded.SaKeyFile {
		return identityErr(
			"consolidate --force: database.sa_key_file does not match the recorded profile for run %s",
			runID,
		)
	}
	return nil
}

func effectiveAuthScheme(db profile.Database) string {
	if db.AuthScheme != "" {
		return db.AuthScheme
	}
	if db.DBMS == "ydb" {
		return config.InferYdbAuthScheme(db)
	}
	return ""
}

func sshMatchesRecorded(live, snap *profile.Profile, liveKnown, snapKnown, runID string) error {
	if live.SSH.User != snap.SSH.User {
		return identityErr(
			"consolidate --force: ssh.user does not match the recorded profile for run %s",
			runID,
		)
	}
	if live.SSH.UseAgent != snap.SSH.UseAgent {
		return identityErr(
			"consolidate --force: ssh.use_agent does not match the recorded profile for run %s",
			runID,
		)
	}
	if live.SSH.InsecureIgnore != snap.SSH.InsecureIgnore {
		return identityErr(
			"consolidate --force: ssh.insecure_ignore_host_key does not match the recorded profile for run %s",
			runID,
		)
	}
	if live.SSH.ConnectTimeout != snap.SSH.ConnectTimeout {
		return identityErr(
			"consolidate --force: ssh.connect_timeout does not match the recorded profile for run %s",
			runID,
		)
	}
	if liveKnown != snapKnown {
		return identityErr(
			"consolidate --force: ssh.known_hosts does not match the recorded profile for run %s",
			runID,
		)
	}
	return nil
}

func namedHosts(items []profile.NamedHost) map[string]struct{} {
	out := make(map[string]struct{}, len(items))
	for _, item := range items {
		out[item.Host] = struct{}{}
	}
	return out
}

func assignmentHosts(items []config.WorkerAssignmentJSON) map[string]struct{} {
	out := make(map[string]struct{}, len(items))
	for _, item := range items {
		out[item.Host] = struct{}{}
	}
	return out
}

func loadAssignmentHosts(items []config.LoadAssignmentJSON) map[string]struct{} {
	out := make(map[string]struct{}, len(items))
	for _, item := range items {
		out[item.Host] = struct{}{}
	}
	return out
}

func sameHostSet(a, b map[string]struct{}) bool {
	if len(a) != len(b) {
		return false
	}
	for host := range a {
		if _, ok := b[host]; !ok {
			return false
		}
	}
	return true
}

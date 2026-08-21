package state

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// Run states per specification §8.10.
const (
	StatePlanned        = "planned"
	StateDeploying      = "deploying"
	StateSchema         = "schema"
	StateLoading        = "loading"
	StateIndexing       = "indexing"
	StateCheckingImport = "checking_import"
	StatePreparing      = "preparing"
	StateArming         = "arming"
	StateRamping        = "ramping"
	StateMeasuring      = "measuring"
	StateDraining       = "draining"
	StateCheckingResult = "checking_result"
	StateCollecting     = "collecting"
	StateConsolidating  = "consolidating"
	StateCompleted      = "completed"
	StateStopping       = "stopping"
	StateFailed         = "failed"
)

var pipelineOrder = map[string]int{
	StatePlanned:        0,
	StateDeploying:      1,
	StateSchema:         2,
	StateLoading:        3,
	StateIndexing:       4,
	StateCheckingImport: 5,
	StatePreparing:      6,
	StateArming:         7,
	StateRamping:        8,
	StateMeasuring:      9,
	StateDraining:       10,
	StateCheckingResult: 11,
	StateCollecting:     12,
	StateConsolidating:  13,
	StateCompleted:      14,
}

// RunState is mutable control-host state for a run.
type RunState struct {
	SchemaVersion   int                `json:"schema_version"`
	RunID           string             `json:"run_id"`
	State           string             `json:"state"`
	UpdatedAt       string             `json:"updated_at"`
	Error           string             `json:"error,omitempty"`
	InsecureHostKey bool               `json:"insecure_ignore_host_key,omitempty"`
	Processes       map[string]Process `json:"processes,omitempty"`
	SkippedSteps    []string           `json:"skipped_steps,omitempty"`
	StartAt         string             `json:"start_at,omitempty"`
}

type Process struct {
	Role          string `json:"role"`
	Host          string `json:"host"`
	Instance      string `json:"instance"`
	PID           int    `json:"pid,omitempty"`
	LaunchPID     int    `json:"launch_pid,omitempty"`
	InstanceNonce string `json:"instance_nonce,omitempty"`
	State         string `json:"state"`
	UpdatedAt     string `json:"updated_at"`
}

// Store manages run-state on the control host.
type Store struct {
	StateDir string
}

// RunDir returns the directory for a specific run.
func (s *Store) RunDir(runID string) string {
	return filepath.Join(s.StateDir, "runs", runID)
}

// StatePath returns run-state.json path.
func (s *Store) StatePath(runID string) string {
	return filepath.Join(s.RunDir(runID), "run-state.json")
}

// ProfileLockPath returns the profile lock file path.
func (s *Store) ProfileLockPath(profileID string) string {
	return filepath.Join(s.StateDir, "profiles", profileID, "run.lock")
}

// Load reads run-state.json.
func (s *Store) Load(runID string) (*RunState, error) {
	path := s.StatePath(runID)
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return &RunState{
				SchemaVersion: 1,
				RunID:         runID,
				State:         StatePlanned,
			}, nil
		}
		return nil, err
	}
	var rs RunState
	if err := json.Unmarshal(data, &rs); err != nil {
		return nil, err
	}
	return &rs, nil
}

// Save atomically writes run-state.json via temp file + rename.
func (s *Store) Save(rs *RunState) error {
	rs.UpdatedAt = time.Now().UTC().Format(time.RFC3339)
	dir := s.RunDir(rs.RunID)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(rs, "", "  ")
	if err != nil {
		return err
	}
	tmp := filepath.Join(dir, "run-state.json.tmp")
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, s.StatePath(rs.RunID))
}

// Transition updates state with validation.
func (s *Store) Transition(runID, newState string) error {
	rs, err := s.Load(runID)
	if err != nil {
		return err
	}
	if err := validateTransition(rs.State, newState); err != nil {
		return err
	}
	rs.State = newState
	if newState == StateFailed {
		// preserve existing error
	}
	return s.Save(rs)
}

// IsTerminal reports whether a state rejects further pipeline transitions.
func IsTerminal(st string) bool {
	return st == StateCompleted || st == StateFailed
}

func validateTransition(oldState, newState string) error {
	if oldState == "" {
		oldState = StatePlanned
	}
	if oldState == newState {
		return nil
	}
	if IsTerminal(oldState) {
		return fmt.Errorf("invalid state transition %s -> %s", oldState, newState)
	}
	if newState == StateFailed || newState == StateStopping {
		return nil
	}
	if oldState == StateStopping {
		return fmt.Errorf("invalid state transition %s -> %s", oldState, newState)
	}
	if allowedRecovery(oldState, newState) {
		return nil
	}
	oldOrder, oldOK := pipelineOrder[oldState]
	newOrder, newOK := pipelineOrder[newState]
	if !oldOK || !newOK {
		return fmt.Errorf("invalid state transition %s -> %s", oldState, newState)
	}
	if newOrder < oldOrder {
		return fmt.Errorf("invalid state transition %s -> %s", oldState, newState)
	}
	return nil
}

// Reached reports whether current is at least as far along the pipeline as min.
func Reached(current, min string) bool {
	curOrder, curOK := pipelineOrder[current]
	minOrder, minOK := pipelineOrder[min]
	return curOK && minOK && curOrder >= minOrder
}

// allowedRecovery permits re-entry into idempotent post stages after the run
// has moved past them. Check itself does not Transition (see orchestrator
// check); collect may still need to re-run after a late check while the run
// sits in consolidating. Indexing recovery covers older runs stuck in the
// former checking_* waypoints.
func allowedRecovery(oldState, newState string) bool {
	switch newState {
	case StateIndexing:
		// EnsureIndexes is idempotent; recovery after a premature/failed check.
		switch oldState {
		case StateCheckingImport, StateCheckingResult, StateDraining, StateCollecting, StateConsolidating:
			return true
		}
	case StateCollecting:
		// Pull check reports written after a late check --after-test.
		return oldState == StateConsolidating
	}
	return false
}

// Fail records failure state and error message.
func (s *Store) Fail(runID string, cause error) error {
	rs, err := s.Load(runID)
	if err != nil {
		return err
	}
	rs.State = StateFailed
	rs.Error = cause.Error()
	return s.Save(rs)
}

// AcquireProfileLock creates an exclusive lock for a profile.
func (s *Store) AcquireProfileLock(profileID, runID string) error {
	dir := filepath.Join(s.StateDir, "profiles", profileID)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	lockPath := s.ProfileLockPath(profileID)
	f, err := os.OpenFile(lockPath, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
	if err != nil {
		if os.IsExist(err) {
			data, readErr := os.ReadFile(lockPath)
			if readErr != nil {
				return fmt.Errorf("profile %s locked by another run", profileID)
			}
			return fmt.Errorf("profile %s locked by run %s", profileID, string(data))
		}
		return err
	}
	if _, err := f.Write([]byte(runID)); err != nil {
		_ = f.Close()
		_ = os.Remove(lockPath)
		return err
	}
	return f.Close()
}

// ReleaseProfileLock removes profile lock if owned by runID.
func (s *Store) ReleaseProfileLock(profileID, runID string) error {
	lockPath := s.ProfileLockPath(profileID)
	data, err := os.ReadFile(lockPath)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	if string(data) != runID {
		return fmt.Errorf("profile lock held by another run")
	}
	return os.Remove(lockPath)
}

// LatestRunID returns the most recently updated run under state/runs, or "".
func (s *Store) LatestRunID() (string, error) {
	root := filepath.Join(s.StateDir, "runs")
	entries, err := os.ReadDir(root)
	if err != nil {
		if os.IsNotExist(err) {
			return "", nil
		}
		return "", err
	}
	var bestID string
	var bestTime time.Time
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		rs, err := s.Load(e.Name())
		if err != nil {
			continue
		}
		t, err := time.Parse(time.RFC3339, rs.UpdatedAt)
		if err != nil {
			info, ierr := e.Info()
			if ierr != nil {
				continue
			}
			t = info.ModTime()
		}
		if bestID == "" || t.After(bestTime) {
			bestID = e.Name()
			bestTime = t
		}
	}
	return bestID, nil
}

// RecordSkip appends a skipped pipeline step name.
func (s *Store) RecordSkip(runID, step string) error {
	rs, err := s.Load(runID)
	if err != nil {
		return err
	}
	for _, existing := range rs.SkippedSteps {
		if existing == step {
			return s.Save(rs)
		}
	}
	rs.SkippedSteps = append(rs.SkippedSteps, step)
	return s.Save(rs)
}

// UpsertProcess stores process supervision metadata.
func (s *Store) UpsertProcess(runID string, proc Process) error {
	rs, err := s.Load(runID)
	if err != nil {
		return err
	}
	if rs.Processes == nil {
		rs.Processes = map[string]Process{}
	}
	proc.UpdatedAt = time.Now().UTC().Format(time.RFC3339)
	key := proc.Role + "/" + proc.Instance
	rs.Processes[key] = proc
	return s.Save(rs)
}

// WriteJSON atomically writes a JSON file in the run directory.
func WriteJSON(dir, name string, v interface{}) error {
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	tmp := filepath.Join(dir, name+".tmp")
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, filepath.Join(dir, name))
}

// ReadJSON reads a JSON file.
func ReadJSON(path string, v interface{}) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	return json.Unmarshal(data, v)
}

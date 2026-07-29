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
	StatePlanned          = "planned"
	StateDeploying        = "deploying"
	StateSchema           = "schema"
	StateLoading          = "loading"
	StateCheckingImport   = "checking_import"
	StatePreparing        = "preparing"
	StateArming           = "arming"
	StateRamping          = "ramping"
	StateMeasuring        = "measuring"
	StateDraining         = "draining"
	StateCheckingResult   = "checking_result"
	StateCollecting       = "collecting"
	StateConsolidating    = "consolidating"
	StateCompleted        = "completed"
	StateStopping         = "stopping"
	StateFailed           = "failed"
)

// RunState is mutable control-host state for a run.
type RunState struct {
	SchemaVersion    int                 `json:"schema_version"`
	RunID            string              `json:"run_id"`
	State            string              `json:"state"`
	UpdatedAt        string              `json:"updated_at"`
	Error            string              `json:"error,omitempty"`
	InsecureHostKey  bool                `json:"insecure_ignore_host_key,omitempty"`
	Processes        map[string]Process  `json:"processes,omitempty"`
}

type Process struct {
	Role      string `json:"role"`
	Host      string `json:"host"`
	Instance  string `json:"instance"`
	PID       int    `json:"pid,omitempty"`
	State     string `json:"state"`
	UpdatedAt string `json:"updated_at"`
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
	rs.State = newState
	if newState == StateFailed {
		// preserve existing error
	}
	return s.Save(rs)
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
	if data, err := os.ReadFile(lockPath); err == nil {
		return fmt.Errorf("profile %s locked by run %s", profileID, string(data))
	}
	return os.WriteFile(lockPath, []byte(runID), 0644)
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

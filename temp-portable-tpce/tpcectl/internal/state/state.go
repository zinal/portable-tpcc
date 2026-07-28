package state

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

const schemaVersion = 1

// RunPhase is the lifecycle state stored locally on the control host.
type RunPhase string

const (
	PhaseStarting  RunPhase = "starting"
	PhaseRunning   RunPhase = "running"
	PhaseStopping  RunPhase = "stopping"
	PhaseCompleted RunPhase = "completed"
	PhaseFailed    RunPhase = "failed"
)

// ProcessRecord tracks one remote process started by tpcectl.
type ProcessRecord struct {
	Role      string    `json:"role"`
	Name      string    `json:"name"`
	Host      string    `json:"host"`
	PID       int       `json:"pid,omitempty"`
	PIDFile   string    `json:"pid_file,omitempty"`
	Output    string    `json:"output"`
	StartedAt time.Time `json:"started_at,omitempty"`
}

// RunState is canonical mutable orchestrator state (spec-orchestrator §14.2).
type RunState struct {
	SchemaVersion   int             `json:"schema_version"`
	RunID           string          `json:"run_id"`
	ProfilePath     string          `json:"profile_path"`
	ProfileSHA256   string          `json:"profile_sha256"`
	RunConfigSHA256 string          `json:"run_config_sha256"`
	RemoteRunConfig string          `json:"remote_run_config"`
	BaseTimeEpoch   int64           `json:"base_time_epoch"`
	StartedAt             time.Time       `json:"started_at,omitempty"`
	MeasurementStartedAt  time.Time       `json:"measurement_started_at,omitempty"`
	State                 RunPhase        `json:"state"`
	Processes       []ProcessRecord `json:"processes,omitempty"`
}

// CurrentRunIndex maps a profile to its active run_id.
type CurrentRunIndex struct {
	RunID string `json:"run_id"`
}

// Store manages local run-state under state-dir.
type Store struct {
	Root string
}

// DefaultStateDir returns XDG or ~/.local/state/tpcectl.
func DefaultStateDir() (string, error) {
	if xdg := os.Getenv("XDG_STATE_HOME"); xdg != "" {
		return filepath.Join(xdg, "tpcectl"), nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".local", "state", "tpcectl"), nil
}

// NewStore creates a store rooted at dir.
func NewStore(dir string) *Store {
	return &Store{Root: dir}
}

// ProfileID derives a stable profile key from name and absolute path.
func ProfileID(r *config.ResolvedProfile) string {
	if r == nil {
		return "unknown"
	}
	safeName := regexp.MustCompile(`[^a-zA-Z0-9._-]+`).ReplaceAllString(r.Name, "_")
	if safeName == "" {
		safeName = "profile"
	}
	sum := sha256.Sum256([]byte(r.ProfilePath))
	return fmt.Sprintf("%s-%s", safeName, hex.EncodeToString(sum[:4]))
}

func (s *Store) runDir(runID string) string {
	return filepath.Join(s.Root, "runs", runID)
}

func (s *Store) runStatePath(runID string) string {
	return filepath.Join(s.runDir(runID), "run-state.json")
}

func (s *Store) profileIndexPath(profileID string) string {
	return filepath.Join(s.Root, "profiles", profileID, "current-run.json")
}

// RunStatePath returns the path to run-state for a run_id.
func (s *Store) RunStatePath(runID string) string {
	return s.runStatePath(runID)
}

// LoadRunState reads run-state.json for runID.
func (s *Store) LoadRunState(runID string) (*RunState, error) {
	data, err := os.ReadFile(s.runStatePath(runID))
	if err != nil {
		return nil, err
	}
	var st RunState
	if err := json.Unmarshal(data, &st); err != nil {
		return nil, fmt.Errorf("parse run-state: %w", err)
	}
	return &st, nil
}

// SaveRunState atomically writes run-state.json with mode 0600.
func (s *Store) SaveRunState(st *RunState) error {
	if st == nil {
		return fmt.Errorf("run state is nil")
	}
	dir := s.runDir(st.RunID)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	st.SchemaVersion = schemaVersion
	data, err := json.MarshalIndent(st, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	tmp := s.runStatePath(st.RunID) + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, s.runStatePath(st.RunID))
}

// SetCurrentRun updates profiles/<id>/current-run.json atomically.
func (s *Store) SetCurrentRun(profileID, runID string) error {
	dir := filepath.Dir(s.profileIndexPath(profileID))
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	idx := CurrentRunIndex{RunID: runID}
	data, err := json.MarshalIndent(idx, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	path := s.profileIndexPath(profileID)
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// ResolveRunID returns run_id from flag or current-run index.
func (s *Store) ResolveRunID(profileID, runID string) (string, error) {
	if runID != "" {
		return runID, nil
	}
	data, err := os.ReadFile(s.profileIndexPath(profileID))
	if err != nil {
		return "", fmt.Errorf("no --run-id and no current run for profile: %w", err)
	}
	var idx CurrentRunIndex
	if err := json.Unmarshal(data, &idx); err != nil {
		return "", err
	}
	if idx.RunID == "" {
		return "", fmt.Errorf("current-run index is empty")
	}
	return idx.RunID, nil
}

// NewRunState seeds initial run-state before remote mutations.
func NewRunState(r *config.ResolvedProfile, runConfigSHA string, baseTimeEpoch int64) (*RunState, error) {
	profileHash, err := fileSHA256(r.ProfilePath)
	if err != nil {
		return nil, err
	}
	return &RunState{
		SchemaVersion:   schemaVersion,
		RunID:           r.EffectiveRunID,
		ProfilePath:     r.ProfilePath,
		ProfileSHA256:   profileHash,
		RunConfigSHA256: runConfigSHA,
		RemoteRunConfig: r.RemoteRunConfigPath(),
		BaseTimeEpoch:   baseTimeEpoch,
		StartedAt:       time.Now().UTC(),
		State:           PhaseStarting,
	}, nil
}

// HasActiveRun reports whether profile has starting/running/stopping state.
func (s *Store) HasActiveRun(profileID string) (bool, *RunState, error) {
	runID, err := s.ResolveRunID(profileID, "")
	if err != nil {
		if os.IsNotExist(err) || strings.Contains(err.Error(), "no --run-id") {
			return false, nil, nil
		}
		return false, nil, err
	}
	st, err := s.LoadRunState(runID)
	if err != nil {
		if os.IsNotExist(err) {
			return false, nil, nil
		}
		return false, nil, err
	}
	switch st.State {
	case PhaseStarting, PhaseRunning, PhaseStopping:
		return true, st, nil
	default:
		return false, st, nil
	}
}

func fileSHA256(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:]), nil
}

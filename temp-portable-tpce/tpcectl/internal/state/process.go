package state

import "time"

// UpsertProcess adds or replaces a process record in run state.
func (st *RunState) UpsertProcess(rec ProcessRecord) {
	for i, p := range st.Processes {
		if p.Role == rec.Role && p.Name == rec.Name {
			st.Processes[i] = rec
			return
		}
	}
	st.Processes = append(st.Processes, rec)
}

// FindProcess returns a process by role and name.
func (st *RunState) FindProcess(role, name string) *ProcessRecord {
	for i := range st.Processes {
		if st.Processes[i].Role == role && st.Processes[i].Name == name {
			return &st.Processes[i]
		}
	}
	return nil
}

// ProcessesByRole returns processes matching role (empty role = all).
func (st *RunState) ProcessesByRole(role string) []ProcessRecord {
	if role == "" {
		return append([]ProcessRecord(nil), st.Processes...)
	}
	var out []ProcessRecord
	for _, p := range st.Processes {
		if p.Role == role {
			out = append(out, p)
		}
	}
	return out
}

// MarkFailed sets state to failed with timestamp preserved.
func (st *RunState) MarkFailed() {
	st.State = PhaseFailed
}

// MarkRunning sets state to running.
func (st *RunState) MarkRunning() {
	st.State = PhaseRunning
}

// MarkStopping sets state to stopping.
func (st *RunState) MarkStopping() {
	st.State = PhaseStopping
}

// MarkCompleted sets state to completed.
func (st *RunState) MarkCompleted() {
	st.State = PhaseCompleted
}

// TouchStarted sets StartedAt if zero.
func (st *RunState) TouchStarted() {
	if st.StartedAt.IsZero() {
		st.StartedAt = time.Now().UTC()
	}
}

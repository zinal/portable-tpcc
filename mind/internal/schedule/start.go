package schedule

import (
	"time"

	"portable-tpcc/mind/internal/config"
)

// Token is written as start-token.json under results/<run_id>/orchestrator/.
type Token struct {
	SchemaVersion   int      `json:"schema_version"`
	RunID           string   `json:"run_id"`
	IssuedAt        string   `json:"issued_at"`
	StartAt         string   `json:"start_at"`
	ExpectedWorkers []string `json:"expected_workers"`
	Phases          Phases   `json:"phases"`
}

type Phases struct {
	RampStart              string `json:"ramp_start"`
	MeasurementStart       string `json:"measurement_start"`
	MeasurementEnd         string `json:"measurement_end"`
	DrainDeadline          string `json:"drain_deadline"`
	AsyncWorkDrainDeadline string `json:"async_work_drain_deadline"`
}

// Compute builds a start token: start_at = now + start_lead_ms.
func Compute(rc *config.RunConfig, now time.Time) Token {
	now = now.UTC()
	startAt := now.Add(time.Duration(rc.Phases.StartLeadMs) * time.Millisecond)
	rampStart := startAt
	measurementStart := rampStart.Add(time.Duration(rc.Phases.RampUpMs) * time.Millisecond)
	measurementEnd := measurementStart.Add(time.Duration(rc.Phases.MeasurementMs) * time.Millisecond)
	drainDeadline := measurementEnd.Add(time.Duration(rc.Phases.TransactionDrainMs) * time.Millisecond)
	asyncDeadline := measurementEnd.Add(time.Duration(rc.Phases.AsyncWorkDrainMs) * time.Millisecond)
	if rc.Phases.AsyncWorkDrainMs == 0 {
		asyncDeadline = drainDeadline
	}
	return Token{
		SchemaVersion:   1,
		RunID:           rc.RunID,
		IssuedAt:        now.Format(time.RFC3339),
		StartAt:         startAt.Format(time.RFC3339),
		ExpectedWorkers: config.ExpectedWorkerNames(rc),
		Phases: Phases{
			RampStart:              rampStart.Format(time.RFC3339),
			MeasurementStart:       measurementStart.Format(time.RFC3339),
			MeasurementEnd:         measurementEnd.Format(time.RFC3339),
			DrainDeadline:          drainDeadline.Format(time.RFC3339),
			AsyncWorkDrainDeadline: asyncDeadline.Format(time.RFC3339),
		},
	}
}

// ParseRFC3339 parses schedule timestamps.
func ParseRFC3339(s string) (time.Time, error) {
	return time.Parse(time.RFC3339, s)
}

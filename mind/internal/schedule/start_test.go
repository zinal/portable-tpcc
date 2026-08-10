package schedule_test

import (
	"testing"
	"time"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/schedule"
)

func TestComputeStartAt(t *testing.T) {
	rc := &config.RunConfig{
		RunID: "run-1",
		Phases: config.PhasesJSON{
			StartLeadMs:        45000,
			RampUpMs:           300000,
			MeasurementMs:      1800000,
			TransactionDrainMs: 30000,
			AsyncWorkDrainMs:   60000,
		},
		WorkerAssignment: []config.WorkerAssignmentJSON{
			{Instance: "worker-a"},
			{Instance: "worker-b"},
		},
	}
	now := time.Date(2026, 7, 28, 11, 59, 30, 0, time.UTC)
	token := schedule.Compute(rc, now)
	if token.StartAt != "2026-07-28T12:00:15Z" {
		t.Fatalf("start_at %s", token.StartAt)
	}
	if token.Phases.MeasurementStart != "2026-07-28T12:05:15Z" {
		t.Fatalf("measurement_start %s", token.Phases.MeasurementStart)
	}
	if len(token.ExpectedWorkers) != 2 {
		t.Fatalf("workers %v", token.ExpectedWorkers)
	}
}

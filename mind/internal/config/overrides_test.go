package config_test

import (
	"strings"
	"testing"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/profile"
)

func TestApplyOverrides_decreasesWarehouses(t *testing.T) {
	p := &profile.Profile{Scale: profile.Scale{Warehouses: 10}}
	w := 4
	if err := config.ApplyOverrides(p, config.ProfileOverrides{Warehouses: &w}); err != nil {
		t.Fatal(err)
	}
	if p.Scale.Warehouses != 4 {
		t.Fatalf("warehouses=%d, want 4", p.Scale.Warehouses)
	}
}

func TestApplyOverrides_rejectsIncrease(t *testing.T) {
	p := &profile.Profile{Scale: profile.Scale{Warehouses: 10}}
	w := 11
	err := config.ApplyOverrides(p, config.ProfileOverrides{Warehouses: &w})
	if err == nil || !strings.Contains(err.Error(), "exceeds profile") {
		t.Fatalf("expected exceed error, got %v", err)
	}
}

func TestApplyOverrides_rejectsNonPositive(t *testing.T) {
	p := &profile.Profile{Scale: profile.Scale{Warehouses: 10}}
	w := 0
	err := config.ApplyOverrides(p, config.ProfileOverrides{Warehouses: &w})
	if err == nil || !strings.Contains(err.Error(), "positive") {
		t.Fatalf("expected positive error, got %v", err)
	}
}

func TestApplyOverrides_phases(t *testing.T) {
	p := &profile.Profile{
		Phases: profile.Phases{RampUp: "5m", Measurement: "120m"},
	}
	ramp := "30s"
	meas := "2m"
	if err := config.ApplyOverrides(p, config.ProfileOverrides{
		RampUp:      &ramp,
		Measurement: &meas,
	}); err != nil {
		t.Fatal(err)
	}
	if p.Phases.RampUp != "30s" || p.Phases.Measurement != "2m" {
		t.Fatalf("phases=%+v", p.Phases)
	}
}

func TestApplyOverrides_rejectsBadDuration(t *testing.T) {
	p := &profile.Profile{Phases: profile.Phases{RampUp: "5m"}}
	bad := "not-a-duration"
	err := config.ApplyOverrides(p, config.ProfileOverrides{RampUp: &bad})
	if err == nil || !strings.Contains(err.Error(), "--ramp-up") {
		t.Fatalf("expected ramp-up parse error, got %v", err)
	}
}

func TestOverridesMatchRunConfig(t *testing.T) {
	rc := &config.RunConfig{
		Scale:  config.ScaleBlock{Warehouses: 5},
		Phases: config.PhasesJSON{RampUpMs: 30000, MeasurementMs: 120000},
	}
	w := 5
	ramp := "30s"
	meas := "2m"
	if err := config.OverridesMatchRunConfig(config.ProfileOverrides{
		Warehouses:  &w,
		RampUp:      &ramp,
		Measurement: &meas,
	}, rc); err != nil {
		t.Fatal(err)
	}
	w2 := 4
	err := config.OverridesMatchRunConfig(config.ProfileOverrides{Warehouses: &w2}, rc)
	if err == nil || !strings.Contains(err.Error(), "conflicts") {
		t.Fatalf("expected conflict, got %v", err)
	}
}

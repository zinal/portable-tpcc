package config

import (
	"fmt"

	"portable-tpcc/mind/internal/profile"
)

// ProfileOverrides are optional CLI replacements for profile fields.
// Nil pointers mean "use the profile value".
type ProfileOverrides struct {
	Warehouses  *int
	RampUp      *string
	Measurement *string
}

// Any reports whether at least one override is set.
func (o ProfileOverrides) Any() bool {
	return o.Warehouses != nil || o.RampUp != nil || o.Measurement != nil
}

// ApplyOverrides mutates p with CLI overrides.
// --warehouses may only decrease (or keep) scale.warehouses from the profile.
func ApplyOverrides(p *profile.Profile, o ProfileOverrides) error {
	if p == nil {
		return fmt.Errorf("nil profile")
	}
	if o.Warehouses != nil {
		w := *o.Warehouses
		if w <= 0 {
			return fmt.Errorf("--warehouses must be positive")
		}
		if w > p.Scale.Warehouses {
			return fmt.Errorf(
				"--warehouses %d exceeds profile scale.warehouses %d",
				w, p.Scale.Warehouses,
			)
		}
		p.Scale.Warehouses = w
	}
	if o.RampUp != nil {
		if _, err := profile.ParseDurationMs(*o.RampUp); err != nil {
			return fmt.Errorf("--ramp-up: %w", err)
		}
		p.Phases.RampUp = *o.RampUp
	}
	if o.Measurement != nil {
		if _, err := profile.ParseDurationMs(*o.Measurement); err != nil {
			return fmt.Errorf("--measurement: %w", err)
		}
		p.Phases.Measurement = *o.Measurement
	}
	return nil
}

// OverridesMatchRunConfig reports whether CLI overrides agree with an already
// materialized run-config. Empty overrides always match.
func OverridesMatchRunConfig(o ProfileOverrides, rc *RunConfig) error {
	if rc == nil || !o.Any() {
		return nil
	}
	if o.Warehouses != nil && rc.Scale.Warehouses != *o.Warehouses {
		return fmt.Errorf(
			"--warehouses=%d conflicts with existing run-config warehouses=%d",
			*o.Warehouses, rc.Scale.Warehouses,
		)
	}
	if o.RampUp != nil {
		ms, err := profile.ParseDurationMs(*o.RampUp)
		if err != nil {
			return fmt.Errorf("--ramp-up: %w", err)
		}
		if rc.Phases.RampUpMs != ms {
			return fmt.Errorf(
				"--ramp-up=%s (%dms) conflicts with existing run-config ramp_up_ms=%d",
				*o.RampUp, ms, rc.Phases.RampUpMs,
			)
		}
	}
	if o.Measurement != nil {
		ms, err := profile.ParseDurationMs(*o.Measurement)
		if err != nil {
			return fmt.Errorf("--measurement: %w", err)
		}
		if rc.Phases.MeasurementMs != ms {
			return fmt.Errorf(
				"--measurement=%s (%dms) conflicts with existing run-config measurement_ms=%d",
				*o.Measurement, ms, rc.Phases.MeasurementMs,
			)
		}
	}
	return nil
}

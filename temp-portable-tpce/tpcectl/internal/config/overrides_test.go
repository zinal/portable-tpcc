package config

import "testing"

func TestApplyRunOverrides(t *testing.T) {
	duration := 60
	users := 4
	base := &ResolvedProfile{
		Profile: Profile{
			Scale: ScaleConfig{DurationSec: 7200},
			StandaloneDriver: &StandaloneDriverConfig{
				Enabled: true, Users: 8, DurationSec: 120,
			},
		},
	}
	out := ApplyRunOverrides(base, RunOverrides{
		DurationSec: &duration,
		Users:       &users,
	})
	if out.Scale.DurationSec != 60 {
		t.Fatalf("duration = %d", out.Scale.DurationSec)
	}
	if out.StandaloneDriver.DurationSec != 60 || out.StandaloneDriver.Users != 4 {
		t.Fatalf("standalone %+v", out.StandaloneDriver)
	}
}

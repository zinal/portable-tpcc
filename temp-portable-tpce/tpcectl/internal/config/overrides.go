package config

// RunOverrides applies ephemeral CLI overrides before a run or smoke test.
type RunOverrides struct {
	DurationSec *int
	Users       *int
}

// ApplyRunOverrides returns a copy of the profile with optional duration/users overrides.
func ApplyRunOverrides(r *ResolvedProfile, o RunOverrides) *ResolvedProfile {
	if r == nil {
		return nil
	}
	if o.DurationSec == nil && o.Users == nil {
		return r
	}
	out := *r
	out.Profile = r.Profile
	if len(r.CE) > 0 {
		out.CE = append([]CEInstance(nil), r.CE...)
	}
	if o.DurationSec != nil {
		out.Scale.DurationSec = *o.DurationSec
		if out.StandaloneDriver != nil && out.StandaloneDriver.Enabled {
			sd := *out.StandaloneDriver
			sd.DurationSec = *o.DurationSec
			out.StandaloneDriver = &sd
		}
	}
	if o.Users != nil {
		if out.StandaloneDriver != nil && out.StandaloneDriver.Enabled {
			sd := *out.StandaloneDriver
			sd.Users = *o.Users
			out.StandaloneDriver = &sd
		}
		for i := range out.CE {
			out.CE[i].Users = *o.Users
		}
	}
	return &out
}

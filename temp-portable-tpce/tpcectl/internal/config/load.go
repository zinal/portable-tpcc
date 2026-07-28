package config

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/google/uuid"
	"gopkg.in/yaml.v3"
)

// Load reads and parses a profile YAML file without template expansion.
func Load(path string) (*Profile, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read profile: %w", err)
	}

	var p Profile
	if err := yaml.Unmarshal(data, &p); err != nil {
		return nil, fmt.Errorf("parse profile YAML: %w", err)
	}

	if p.APIVersion != "" && p.APIVersion != APIVersion {
		return nil, fmt.Errorf("unsupported apiVersion %q (want %s)", p.APIVersion, APIVersion)
	}
	if p.Kind != "" && p.Kind != "Profile" {
		return nil, fmt.Errorf("unsupported kind %q (want Profile)", p.Kind)
	}

	return &p, nil
}

// Resolve applies defaults, validates templates, expands placeholders, and resolves host addresses.
func Resolve(p *Profile, profilePath string, runID string) (*ResolvedProfile, error) {
	if p == nil {
		return nil, fmt.Errorf("profile is nil")
	}

	applyDefaults(p)

	if err := scanProfileTemplates(p); err != nil {
		return nil, err
	}

	effectiveRunID := runID
	if effectiveRunID == "" {
		effectiveRunID = p.RunID
	}
	if effectiveRunID == "" {
		effectiveRunID = defaultRunID()
	}

	absProfile, err := filepath.Abs(profilePath)
	if err != nil {
		return nil, fmt.Errorf("resolve profile path: %w", err)
	}
	profileDir := filepath.Dir(absProfile)

	vars := map[string]string{
		"run_id":     effectiveRunID,
		"local_bin":  absPath(profileDir, p.Paths.LocalBin),
		"local_data": absPath(profileDir, p.Paths.LocalData),
		"local_sql":  absPath(profileDir, p.Paths.LocalSQL),
	}

	expanded, err := expandProfileCopy(*p, vars)
	if err != nil {
		return nil, err
	}

	hostAddresses := make(map[string]string, len(expanded.Hosts))
	for name, host := range expanded.Hosts {
		if strings.TrimSpace(host.Address) == "" {
			return nil, fmt.Errorf("hosts.%s.address is required", name)
		}
		hostAddresses[name] = host.Address
	}

	return &ResolvedProfile{
		Profile:        expanded,
		ProfilePath:    absProfile,
		AbsolutePaths:  expanded.Paths,
		EffectiveRunID: effectiveRunID,
		HostAddresses:  hostAddresses,
	}, nil
}

func applyDefaults(p *Profile) {
	if p.Scale.ActiveCustomers == 0 && p.Scale.Customers > 0 {
		p.Scale.ActiveCustomers = p.Scale.Customers
	}
	if p.BaseTimeLeadSec == 0 {
		p.BaseTimeLeadSec = 30
	}
	if p.Timeouts.ConfigDistribute == 0 {
		p.Timeouts.ConfigDistribute = 30 * time.Second
	}
	if p.Timeouts.Ready == 0 {
		p.Timeouts.Ready = 60 * time.Second
	}
	if p.Timeouts.CleanupWait == 0 {
		p.Timeouts.CleanupWait = 300 * time.Second
	}
	if p.Timeouts.CECompletionGrace == 0 {
		p.Timeouts.CECompletionGrace = 30 * time.Minute
	}
	if p.Timeouts.MEEDrain == 0 {
		p.Timeouts.MEEDrain = 10 * time.Second
	}
	if p.Timeouts.StopGrace == 0 {
		p.Timeouts.StopGrace = 30 * time.Second
	}
	if p.SSH.Port == 0 {
		p.SSH.Port = 22
	}
	if p.SSH.ConnectTimeout == 0 {
		p.SSH.ConnectTimeout = 15 * time.Second
	}
	if p.DB.SSLMode == "" {
		p.DB.SSLMode = "prefer"
	}
	if p.Schema.Mode == "" {
		p.Schema.Mode = "base"
	}
	if p.Schema.Partitions == 0 {
		p.Schema.Partitions = 32
	}
	if !p.Scale.ClientSide {
		p.Scale.ClientSide = true
	}
}

func defaultRunID() string {
	return time.Now().UTC().Format("20060102T150405Z") + "-" + uuid.NewString()[:8]
}

func absPath(base, p string) string {
	if p == "" {
		return ""
	}
	if filepath.IsAbs(p) {
		return filepath.Clean(p)
	}
	return filepath.Clean(filepath.Join(base, p))
}

func scanProfileTemplates(p *Profile) error {
	check := func(field, value string) error {
		if value == "" {
			return nil
		}
		if err := ScanTemplates(value); err != nil {
			return fmt.Errorf("%s: %w", field, err)
		}
		return nil
	}

	for i, a := range p.Deploy.Artifacts {
		if err := check(fmt.Sprintf("deploy.artifacts[%d].src", i), a.Src); err != nil {
			return err
		}
		if err := check(fmt.Sprintf("deploy.artifacts[%d].dst", i), a.Dst); err != nil {
			return err
		}
	}
	for i, bh := range p.BH {
		if err := check(fmt.Sprintf("bh[%d].output", i), bh.Output); err != nil {
			return err
		}
	}
	for i, mee := range p.MEE {
		if err := check(fmt.Sprintf("mee[%d].output", i), mee.Output); err != nil {
			return err
		}
	}
	if p.DM != nil {
		if err := check("dm.output", p.DM.Output); err != nil {
			return err
		}
	}
	for i, ce := range p.CE {
		if err := check(fmt.Sprintf("ce[%d].output", i), ce.Output); err != nil {
			return err
		}
	}
	if p.StandaloneDriver != nil {
		if err := check("standalone_driver.output", p.StandaloneDriver.Output); err != nil {
			return err
		}
	}
	if err := check("collect.dest", p.Collect.Dest); err != nil {
		return err
	}
	return nil
}

func expandProfileCopy(p Profile, vars map[string]string) (Profile, error) {
	var err error
	expand := func(s string) string {
		if err != nil || s == "" {
			return s
		}
		out, e := ExpandTemplates(s, vars)
		if e != nil {
			err = e
			return s
		}
		return out
	}

	for i := range p.Deploy.Artifacts {
		p.Deploy.Artifacts[i].Src = expand(p.Deploy.Artifacts[i].Src)
		p.Deploy.Artifacts[i].Dst = expand(p.Deploy.Artifacts[i].Dst)
	}
	for i := range p.BH {
		p.BH[i].Output = expand(p.BH[i].Output)
	}
	for i := range p.MEE {
		p.MEE[i].Output = expand(p.MEE[i].Output)
	}
	if p.DM != nil {
		p.DM.Output = expand(p.DM.Output)
	}
	for i := range p.CE {
		p.CE[i].Output = expand(p.CE[i].Output)
	}
	if p.StandaloneDriver != nil {
		p.StandaloneDriver.Output = expand(p.StandaloneDriver.Output)
	}
	p.Collect.Dest = expand(p.Collect.Dest)

	p.Paths.LocalBin = expand(p.Paths.LocalBin)
	p.Paths.LocalData = expand(p.Paths.LocalData)
	p.Paths.LocalSQL = expand(p.Paths.LocalSQL)

	if err != nil {
		return Profile{}, err
	}
	return p, nil
}

// HostAddress returns the network address for a logical host name.
func (r *ResolvedProfile) HostAddress(name string) (string, error) {
	addr, ok := r.HostAddresses[name]
	if !ok || addr == "" {
		return "", fmt.Errorf("unknown host %q", name)
	}
	return addr, nil
}

// RuntimeHosts returns unique host names that run BH/MEE/DM/CE/standalone processes.
func (r *ResolvedProfile) RuntimeHosts() []string {
	seen := make(map[string]struct{})
	add := func(name string) {
		if name != "" {
			seen[name] = struct{}{}
		}
	}
	for _, bh := range r.BH {
		add(bh.Host)
	}
	for _, mee := range r.MEE {
		add(mee.Host)
	}
	if r.DM != nil {
		add(r.DM.Host)
	}
	for _, ce := range r.CE {
		add(ce.Host)
	}
	if r.StandaloneDriver != nil && r.StandaloneDriver.Enabled {
		add(r.StandaloneDriver.Host)
	}
	out := make([]string, 0, len(seen))
	for name := range seen {
		out = append(out, name)
	}
	return out
}

// RemoteRunConfigPath returns the shared run-config path on runtime hosts.
func (r *ResolvedProfile) RemoteRunConfigPath() string {
	return filepath.ToSlash(filepath.Join(r.Paths.RemoteRoot, "runs", r.EffectiveRunID, "run-config.json"))
}

// EffectiveDurationSec returns measurement duration for CE or standalone mode.
func (r *ResolvedProfile) EffectiveDurationSec() int {
	if r.StandaloneDriver != nil && r.StandaloneDriver.Enabled {
		return r.StandaloneDriver.DurationSec
	}
	return r.Scale.DurationSec
}

// UsesStandalone reports whether the profile runs a single combined Driver.
func (r *ResolvedProfile) UsesStandalone() bool {
	return r.StandaloneDriver != nil && r.StandaloneDriver.Enabled
}

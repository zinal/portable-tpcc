package remote

import (
	"fmt"
	"strings"
	"time"

	"portable-tpcc/tpccctl/internal/profile"
)

// IsLoopback reports whether address should use a Local session.
func IsLoopback(address string) bool {
	a := strings.ToLower(strings.TrimSpace(address))
	a = strings.TrimSuffix(a, ":22")
	return a == "127.0.0.1" || a == "localhost" || a == "::1" || a == "[::1]"
}

// Dial opens a Session for a profile host entry.
func Dial(hostKey string, entry profile.HostEntry, cfg DialConfig) (Session, error) {
	if entry.Address == "" {
		return nil, fmt.Errorf("host %s has empty address", hostKey)
	}
	if IsLoopback(entry.Address) {
		return NewLocal(hostKey, entry.Address, cfg.LocalRoot)
	}
	return DialSSH(hostKey, entry.Address, cfg)
}

// DialConfigFromProfile builds DialConfig from profile + expanded paths.
func DialConfigFromProfile(p *profile.Profile, knownHosts, localRoot string) (DialConfig, error) {
	timeout := 10 * time.Second
	if p.SSH.ConnectTimeout != "" {
		d, err := time.ParseDuration(p.SSH.ConnectTimeout)
		if err != nil {
			return DialConfig{}, fmt.Errorf("ssh.connect_timeout: %w", err)
		}
		timeout = d
	}
	return DialConfig{
		User:               p.SSH.User,
		KnownHostsPath:     knownHosts,
		InsecureIgnoreHost: p.SSH.InsecureIgnore,
		ConnectTimeout:     timeout,
		LocalRoot:          localRoot,
		UseAgent:           p.SSH.UseAgent,
	}, nil
}

// UniqueHostKeys returns host keys referenced by loaders and workers.
func UniqueHostKeys(p *profile.Profile) []string {
	seen := map[string]bool{}
	var out []string
	add := func(host string) {
		if host == "" || seen[host] {
			return
		}
		seen[host] = true
		out = append(out, host)
	}
	for _, l := range p.Loaders {
		add(l.Host)
	}
	for _, w := range p.Workers {
		add(w.Host)
	}
	return out
}

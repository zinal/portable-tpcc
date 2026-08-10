package remote

import (
	"fmt"
	"strings"
	"time"

	"portable-tpcc/mind/internal/profile"
)

// IsLoopback reports whether address should use a Local session.
func IsLoopback(address string) bool {
	a := strings.ToLower(strings.TrimSpace(address))
	a = strings.TrimSuffix(a, ":22")
	return a == "127.0.0.1" || a == "localhost" || a == "::1" || a == "[::1]"
}

// Dial opens a Session for a loader/worker host address.
// host is both the session key and the network address; identical host strings
// across profile instances share one session (co-location).
func Dial(host string, cfg DialConfig) (Session, error) {
	if host == "" {
		return nil, fmt.Errorf("host address is empty")
	}
	if IsLoopback(host) {
		return NewLocal(host, host, cfg.LocalRoot)
	}
	return DialSSH(host, host, cfg)
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

// UniqueHosts returns distinct host addresses referenced by loaders and workers.
// Duplicate addresses (co-located instances) appear once.
func UniqueHosts(p *profile.Profile) []string {
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

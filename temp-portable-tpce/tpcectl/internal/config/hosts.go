package config

// DeployHosts returns unique host names that receive deployment artifacts (§8).
func (r *ResolvedProfile) DeployHosts() []string {
	seen := make(map[string]struct{})
	add := func(name string) {
		if name != "" {
			seen[name] = struct{}{}
		}
	}
	for _, name := range r.RuntimeHosts() {
		add(name)
	}
	for _, shard := range r.Load.Shards {
		add(shard.Host)
	}
	out := make([]string, 0, len(seen))
	for name := range seen {
		out = append(out, name)
	}
	return out
}

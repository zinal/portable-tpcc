package remote_test

import (
	"testing"

	"portable-tpcc/mind/internal/profile"
	"portable-tpcc/mind/internal/remote"
)

func TestUniqueHostsCollapsesCoLocatedAddresses(t *testing.T) {
	p := &profile.Profile{
		Loaders: []profile.NamedHost{
			{Name: "loader-a", Host: "10.10.0.21"},
			{Name: "loader-b", Host: "10.10.0.21"},
		},
		Workers: []profile.NamedHost{
			{Name: "worker-a", Host: "10.10.0.21"},
			{Name: "worker-b", Host: "10.10.0.22"},
		},
	}
	got := remote.UniqueHosts(p)
	if len(got) != 2 {
		t.Fatalf("UniqueHosts=%v, want 2 distinct addresses", got)
	}
	seen := map[string]bool{}
	for _, h := range got {
		seen[h] = true
	}
	if !seen["10.10.0.21"] || !seen["10.10.0.22"] {
		t.Fatalf("UniqueHosts=%v", got)
	}
}

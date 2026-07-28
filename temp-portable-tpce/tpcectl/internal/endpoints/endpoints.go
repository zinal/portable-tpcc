package endpoints

import (
	"fmt"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

// Sets holds BH and MEE endpoint lists for run-config (spec-orchestrator §9.2).
type Sets struct {
	BH  []string
	MEE []string
}

// Build constructs endpoint_sets from a resolved profile using host addresses.
func Build(r *config.ResolvedProfile) (Sets, error) {
	if r == nil {
		return Sets{}, fmt.Errorf("profile is nil")
	}

	bh := make([]string, 0, len(r.BH))
	for _, inst := range r.BH {
		addr, err := r.HostAddress(inst.Host)
		if err != nil {
			return Sets{}, fmt.Errorf("bh %s: %w", inst.Name, err)
		}
		bh = append(bh, fmt.Sprintf("%s:%d", addr, inst.Listen))
	}

	mee := make([]string, 0, len(r.MEE))
	for _, inst := range r.MEE {
		addr, err := r.HostAddress(inst.Host)
		if err != nil {
			return Sets{}, fmt.Errorf("mee %s: %w", inst.Name, err)
		}
		mee = append(mee, fmt.Sprintf("%s:%d", addr, inst.Listen))
	}

	if len(bh) == 0 {
		return Sets{}, fmt.Errorf("endpoint_sets.bh must not be empty")
	}
	if len(mee) == 0 {
		return Sets{}, fmt.Errorf("endpoint_sets.mee must not be empty")
	}

	return Sets{BH: bh, MEE: mee}, nil
}

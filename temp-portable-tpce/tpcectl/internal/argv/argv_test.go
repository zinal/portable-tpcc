package argv_test

import (
	"strings"
	"testing"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/argv"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

func TestBuildAllIncludesRunConfigAndRoles(t *testing.T) {
	r := &config.ResolvedProfile{
		Profile: config.Profile{
			Paths: config.PathsConfig{RemoteRoot: "/opt/tpce"},
			DM:    &config.DMInstance{Name: "dm0", Host: "h1", Output: "/opt/tpce/runs/x/dm0"},
			BH:    []config.BHInstance{{Name: "bh1", Host: "h1", Listen: 30000, Output: "/opt/tpce/runs/x/bh1"}},
			MEE:   []config.MEEInstance{{Name: "mee1", Host: "h1", Listen: 30010, UniqueID: 2, Output: "/opt/tpce/runs/x/mee1"}},
			CE:    []config.CEInstance{{Name: "ce1", Host: "h2", Users: 4, CEIDBase: 10, Output: "/opt/tpce/runs/x/ce1"}},
		},
		EffectiveRunID: "x",
		HostAddresses: map[string]string{"h1": "10.0.0.1", "h2": "10.0.0.2"},
	}
	inst, err := argv.BuildAll(r)
	if err != nil {
		t.Fatalf("build argv: %v", err)
	}
	if len(inst) != 4 {
		t.Fatalf("expected 4 instances, got %d", len(inst))
	}
	joined := ""
	for _, i := range inst {
		joined += i.Command + "\n"
	}
	for _, want := range []string{"--run-config", "--role dm", "--role ce", "-U 2", "--ce-id-base 10"} {
		if !strings.Contains(joined, want) {
			t.Fatalf("missing %q in commands:\n%s", want, joined)
		}
	}
}

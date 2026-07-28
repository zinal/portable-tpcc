package load

import (
	"strings"
	"testing"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

func TestLoaderCommand(t *testing.T) {
	t.Setenv("TPCE_PGPASSWORD", "secret")
	profile := &config.ResolvedProfile{
		Profile: config.Profile{
			Paths: config.PathsConfig{RemoteRoot: "/opt/tpce"},
			Scale: config.ScaleConfig{
				Customers:        50000,
				ScaleFactor:      500,
				InitialTradeDays: 300,
			},
			DB: config.DBConfig{
				Host:        "db1.example",
				Port:        5432,
				Name:        "tpce",
				User:        "tpce",
				PasswordEnv: "TPCE_PGPASSWORD",
				SSLMode:     "prefer",
			},
			Load: config.LoadConfig{
				Shards: []config.LoadShard{
					{Host: "load1", Begin: 1, Count: 25000},
				},
			},
		},
	}
	cmd, err := LoaderCommand(profile, profile.Load.Shards[0])
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		"bin/Loader.exe",
		"-l CUSTOM",
		"-i /opt/tpce/data",
		"-b 1",
		"-c 25000",
		"-t 50000",
		"-f 500",
		"-w 300",
		"host=db1.example",
	} {
		if !strings.Contains(cmd, want) {
			t.Fatalf("command missing %q: %s", want, cmd)
		}
	}
	if strings.Contains(cmd, "password=") {
		t.Fatalf("password must not appear in conninfo: %s", cmd)
	}
}

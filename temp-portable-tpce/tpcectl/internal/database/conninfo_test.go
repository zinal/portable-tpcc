package database

import (
	"strings"
	"testing"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

func TestConnInfoWithoutPassword(t *testing.T) {
	profile := &config.ResolvedProfile{
		Profile: config.Profile{
			DB: config.DBConfig{
				Host:    "db1.example",
				Port:    5432,
				Name:    "tpce",
				User:    "tpce",
				SSLMode: "prefer",
			},
		},
	}
	got, err := ConnInfo(profile, false)
	if err != nil {
		t.Fatal(err)
	}
	want := "host=db1.example port=5432 dbname=tpce user=tpce sslmode=prefer"
	if got != want {
		t.Fatalf("conninfo = %q, want %q", got, want)
	}
}

func TestConnInfoWithPassword(t *testing.T) {
	t.Setenv("TPCE_PGPASSWORD", "s3cr'et")
	profile := &config.ResolvedProfile{
		Profile: config.Profile{
			DB: config.DBConfig{
				Host:        "localhost",
				Port:        5432,
				Name:        "tpce",
				User:        "tpce",
				PasswordEnv: "TPCE_PGPASSWORD",
				SSLMode:     "disable",
			},
		},
	}
	got, err := ConnInfo(profile, true)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(got, "password='s3cr\\'et'") || !strings.Contains(got, "host=localhost") {
		t.Fatalf("conninfo = %q", got)
	}
}

func TestConnInfoMissingPasswordEnv(t *testing.T) {
	profile := &config.ResolvedProfile{
		Profile: config.Profile{
			DB: config.DBConfig{
				Host:        "localhost",
				Port:        5432,
				Name:        "tpce",
				User:        "tpce",
				PasswordEnv: "MISSING",
			},
		},
	}
	_, err := ConnInfo(profile, true)
	if err == nil {
		t.Fatal("expected error for missing password env")
	}
}

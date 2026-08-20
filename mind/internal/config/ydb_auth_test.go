package config_test

import (
	"testing"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/profile"
)

func TestInferYdbAuthScheme(t *testing.T) {
	cases := []struct {
		name string
		db   profile.Database
		want string
	}{
		{name: "explicit", db: profile.Database{AuthScheme: "sa_key"}, want: "sa_key"},
		{name: "sa_key_file", db: profile.Database{SaKeyFile: "k.json"}, want: "sa_key"},
		{name: "login", db: profile.Database{User: "root", PasswordEnv: "P"}, want: "login"},
		{name: "anonymous", db: profile.Database{}, want: "anonymous"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := config.InferYdbAuthScheme(tc.db); got != tc.want {
				t.Fatalf("got %q want %q", got, tc.want)
			}
		})
	}
}

func TestBuildRunConfig_rewritesYdbCredentialPaths(t *testing.T) {
	seed := int64(1)
	p := &profile.Profile{
		Metadata: profile.Metadata{Name: "ydb-auth"},
		Database: profile.Database{
			DBMS:       "ydb",
			Endpoint:   "grpcs://ydb.example.net:2135",
			Database:   "/Root/tpcc",
			Path:       "tpcc",
			AuthScheme: "sa_key",
			SaKeyFile:  "/control/secrets/sa.json",
			CaFile:     "/control/certs/root.pem",
		},
		Scale:   profile.Scale{Warehouses: 1},
		Data:    profile.Data{Seed: &seed, BatchRows: 100},
		Loaders: []profile.NamedHost{{Name: "loader-a", Host: "h1"}},
		Workers: []profile.NamedHost{{Name: "worker-a", Host: "h1"}},
		Phases: profile.Phases{
			StartLead:        "1s",
			RampUp:           "1s",
			Measurement:      "120m",
			TransactionDrain: "1s",
			StopGrace:        "1s",
			MaxClockSkew:     "100ms",
		},
		Runtime: profile.Runtime{
			ThreadsPerWorker:     1,
			MaxInflightPerWorker: 8,
		},
	}
	rc, err := config.BuildRunConfig(config.BuildInput{
		Profile: p,
		RunID:   "run-1",
	})
	if err != nil {
		t.Fatal(err)
	}
	if rc.Database.AuthScheme != "sa_key" {
		t.Fatalf("auth_scheme=%q", rc.Database.AuthScheme)
	}
	if rc.Database.CaFile != config.RemoteCAFileName {
		t.Fatalf("ca_file=%q want %q", rc.Database.CaFile, config.RemoteCAFileName)
	}
	if rc.Database.SaKeyFile != config.RemoteSAKeyFileName {
		t.Fatalf("sa_key_file=%q want %q", rc.Database.SaKeyFile, config.RemoteSAKeyFileName)
	}
	if rc.Database.PasswordFile != "" || rc.Database.PasswordEnv != "" {
		t.Fatalf("sa_key run-config must omit password fields, got file=%q env=%q",
			rc.Database.PasswordFile, rc.Database.PasswordEnv)
	}
}

func TestBuildRunConfig_usesPasswordFileNotEnv(t *testing.T) {
	seed := int64(1)
	p := &profile.Profile{
		Metadata: profile.Metadata{Name: "pg"},
		Database: profile.Database{
			DBMS:        "pgsql",
			Endpoint:    "localhost:5432",
			Database:    "tpcc",
			Path:        "portable_tpcc",
			User:        "bench",
			PasswordEnv: "TPCC_PASSWORD",
		},
		Scale:   profile.Scale{Warehouses: 1},
		Data:    profile.Data{Seed: &seed, BatchRows: 100},
		Loaders: []profile.NamedHost{{Name: "loader-a", Host: "h1"}},
		Workers: []profile.NamedHost{{Name: "worker-a", Host: "h1"}},
		Phases: profile.Phases{
			StartLead:        "1s",
			RampUp:           "1s",
			Measurement:      "120m",
			TransactionDrain: "1s",
			StopGrace:        "1s",
			MaxClockSkew:     "100ms",
		},
		Runtime: profile.Runtime{
			ThreadsPerWorker:     1,
			MaxInflightPerWorker: 8,
		},
	}
	rc, err := config.BuildRunConfig(config.BuildInput{
		Profile: p,
		RunID:   "run-1",
	})
	if err != nil {
		t.Fatal(err)
	}
	if rc.Database.PasswordFile != config.RemotePasswordFileName {
		t.Fatalf("password_file=%q want %q", rc.Database.PasswordFile, config.RemotePasswordFileName)
	}
	if rc.Database.PasswordEnv != "" {
		t.Fatalf("password_env=%q, want empty in orchestrated run-config", rc.Database.PasswordEnv)
	}
	if rc.Database.User != "bench" {
		t.Fatalf("user=%q want bench", rc.Database.User)
	}
}

func TestNeedsRemotePasswordFile(t *testing.T) {
	if !config.NeedsRemotePasswordFile(profile.Database{DBMS: "pgsql", PasswordEnv: "P"}) {
		t.Fatal("pgsql with password_env should need file")
	}
	if config.NeedsRemotePasswordFile(profile.Database{DBMS: "ydb", AuthScheme: "sa_key", PasswordEnv: ""}) {
		t.Fatal("ydb sa_key should not need password file")
	}
	if !config.NeedsRemotePasswordFile(profile.Database{
		DBMS: "ydb", AuthScheme: "login", User: "root", PasswordEnv: "P",
	}) {
		t.Fatal("ydb login should need password file")
	}
}

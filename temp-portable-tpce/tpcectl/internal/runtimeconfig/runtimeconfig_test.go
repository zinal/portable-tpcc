package runtimeconfig_test

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/runtimeconfig"
)

func TestBuildDeterministicJSON(t *testing.T) {
	r := &config.ResolvedProfile{
		Profile: config.Profile{
			Name: "lab",
			Paths: config.PathsConfig{
				RemoteRoot: "/opt/tpce",
			},
			Scale: config.ScaleConfig{
				Customers:        5000,
				ActiveCustomers:  5000,
				ScaleFactor:      500,
				InitialTradeDays: 300,
				DurationSec:      60,
				ClientSide:       true,
			},
			DB: config.DBConfig{
				Host: "localhost", Port: 5432, Name: "tpce", User: "tpce",
				PasswordEnv: "TPCE_PGPASSWORD", SSLMode: "prefer",
			},
			BH:  []config.BHInstance{{Name: "bh1", Host: "h1", Listen: 30000}},
			MEE: []config.MEEInstance{{Name: "mee1", Host: "h1", Listen: 30010, UniqueID: 1}},
		},
		EffectiveRunID: "run-1",
		HostAddresses:  map[string]string{"h1": "127.0.0.1"},
	}

	now := time.Unix(1_700_000_000, 0).UTC()
	epoch := int64(1_800_000_000)
	_, raw1, hash1, err := runtimeconfig.Build(r, runtimeconfig.BuildOptions{Now: now, BaseTimeEpoch: &epoch})
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	_, raw2, hash2, err := runtimeconfig.Build(r, runtimeconfig.BuildOptions{Now: now, BaseTimeEpoch: &epoch})
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	if hash1 != hash2 {
		t.Fatalf("hashes differ: %s vs %s", hash1, hash2)
	}
	if string(raw1) != string(raw2) {
		t.Fatalf("json bytes differ")
	}

	var doc map[string]any
	if err := json.Unmarshal(raw1, &doc); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if doc["schema_version"].(float64) != 1 {
		t.Fatalf("unexpected schema_version: %v", doc["schema_version"])
	}
	if !strings.Contains(string(raw1), `"endpoint_sets"`) {
		t.Fatal("missing endpoint_sets")
	}
}

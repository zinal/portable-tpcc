package profile_test

import (
	"strings"
	"testing"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/profile"
	"portable-tpcc/mind/internal/validate"
)

func TestExample_validForEachDBMS(t *testing.T) {
	for _, dbms := range []string{"pgsql", "ydb", "oceanbase"} {
		p, err := profile.ExampleWithName(dbms, "example-"+dbms, "tpcc")
		if err != nil {
			t.Fatalf("%s: %v", dbms, err)
		}
		data, err := profile.EncodeExample(p)
		if err != nil {
			t.Fatalf("%s encode: %v", dbms, err)
		}
		parsed, err := profile.Parse(data)
		if err != nil {
			t.Fatalf("%s parse generated YAML: %v\n%s", dbms, err, data)
		}
		res := validate.Profile(parsed)
		if !res.Valid {
			t.Fatalf("%s generated profile invalid: %v\n%s", dbms, res.Errors, data)
		}
		if !res.TPCCSettingsConformant {
			t.Fatalf("%s expected TPC-C-conformant defaults, deviations: %v", dbms, res.TPCCSettingsDeviations)
		}
		if len(parsed.Loaders) != 1 || parsed.Loaders[0].Host != "localhost" {
			t.Fatalf("%s loaders=%+v, want [localhost]", dbms, parsed.Loaders)
		}
		if len(parsed.Workers) != 1 || parsed.Workers[0].Host != "localhost" {
			t.Fatalf("%s workers=%+v, want [localhost]", dbms, parsed.Workers)
		}
		got := config.ResolveWorkload(parsed.Workload)
		want := config.DefaultWorkload()
		if got != want {
			t.Fatalf("%s workload %+v != defaults %+v", dbms, got, want)
		}
	}
}

func TestExample_pgsqlIncludesOptions(t *testing.T) {
	p, err := profile.Example("pgsql")
	if err != nil {
		t.Fatal(err)
	}
	data, err := profile.EncodeExample(p)
	if err != nil {
		t.Fatal(err)
	}
	text := string(data)
	for _, want := range []string{
		"dbms: pgsql",
		"endpoint: localhost:5432",
		"user: postgres",
		"password_env: TPCC_PASSWORD",
		"partitioning: none",
		"foreign_keys: on",
		"histogram:",
		"unit: us",
		"highest: 120000000",
		"after_import: false",
		"include_events: false",
		"async_work_drain: 30s",
		"seed: 1",
		"batch_rows: 10000",
		"terminals_per_warehouse: 10",
		"max_inflight_per_worker: 100",
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("pgsql example missing %q\n%s", want, text)
		}
	}
	if strings.Contains(text, "auth_scheme:") {
		t.Fatalf("pgsql example must not include YDB auth_scheme:\n%s", text)
	}
	if strings.Contains(text, "partition_count:") {
		t.Fatalf("pgsql default partitioning=none must omit partition_count:\n%s", text)
	}
}

func TestExample_ydbAnonymousOmitsLoginFields(t *testing.T) {
	p, err := profile.Example("ydb")
	if err != nil {
		t.Fatal(err)
	}
	data, err := profile.EncodeExample(p)
	if err != nil {
		t.Fatal(err)
	}
	text := string(data)
	for _, want := range []string{
		"dbms: ydb",
		"endpoint: localhost:2136",
		"database: /local",
		"path: tpcc",
		"auth_scheme: anonymous",
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("ydb example missing %q\n%s", want, text)
		}
	}
	for _, reject := range []string{
		"password_env:",
		"sa_key_file:",
		"options:",
	} {
		if strings.Contains(text, reject) {
			t.Fatalf("ydb anonymous example must not include %q\n%s", reject, text)
		}
	}
	if p.Database.User != "" || p.Database.PasswordEnv != "" || p.Database.SaKeyFile != "" {
		t.Fatalf("ydb anonymous must omit login/sa_key fields: %+v", p.Database)
	}
}

func TestExample_oceanbaseIncludesOptions(t *testing.T) {
	p, err := profile.Example("oceanbase")
	if err != nil {
		t.Fatal(err)
	}
	data, err := profile.EncodeExample(p)
	if err != nil {
		t.Fatal(err)
	}
	text := string(data)
	for _, want := range []string{
		"dbms: oceanbase",
		"endpoint: localhost:2881",
		"user: root@root",
		"partitions: 0",
		"foreign_keys: on",
		"query_timeout: 600",
		"index_parallel: 4",
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("oceanbase example missing %q\n%s", want, text)
		}
	}
}

func TestExample_unknownDBMS(t *testing.T) {
	if _, err := profile.Example("mysql"); err == nil {
		t.Fatal("expected error for unknown dbms")
	}
}

func TestNameFromProfilePath(t *testing.T) {
	cases := map[string]string{
		"profile.yaml":         "profile",
		"/tmp/Lab.YDB.v1.yaml": "lab-ydb-v1",
		"10warehouses.yaml":    "p-10warehouses",
		"":                     "tpcc",
	}
	for in, want := range cases {
		if got := profile.NameFromProfilePath(in); got != want {
			t.Errorf("NameFromProfilePath(%q)=%q, want %q", in, got, want)
		}
	}
}

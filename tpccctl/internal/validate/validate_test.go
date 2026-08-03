package validate_test

import (
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/tpccctl/internal/profile"
	"portable-tpcc/tpccctl/internal/validate"
)

func TestValidate_validProfile(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	res := validate.Profile(p)
	if !res.Valid {
		t.Fatalf("expected valid profile, errors: %v", res.Errors)
	}
	// Engineering fixture uses 30m measurement — structural OK, TPC-C settings deviate.
	if res.TPCCSettingsConformant {
		t.Fatal("expected tpcc_settings_conformant=false for 30m measurement fixture")
	}
	if len(res.TPCCSettingsDeviations) == 0 {
		t.Fatal("expected TPC-C settings deviations for engineering fixture")
	}
}

func TestValidate_tpccSettingsConformantWhenDefaultsMatch(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	p.Phases.Measurement = "120m"
	res := validate.Profile(p)
	if !res.Valid {
		t.Fatalf("expected valid profile, errors: %v", res.Errors)
	}
	if !res.TPCCSettingsConformant {
		t.Fatalf("expected conformant settings, deviations: %v", res.TPCCSettingsDeviations)
	}
}

func TestValidate_tpccSettingsDeviationsDoNotFail(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	p.Workload.TerminalsPerWarehouse = 20
	p.Runtime.Pacing = "disabled"
	p.Runtime.ThinkTimeDistribution = "compatibility"
	p.Phases.Measurement = "30m"
	res := validate.Profile(p)
	if !res.Valid {
		t.Fatalf("TPC-C deviations must not fail structural validate, errors: %v", res.Errors)
	}
	if res.TPCCSettingsConformant {
		t.Fatal("expected non-conformant settings")
	}
	joined := strings.Join(res.TPCCSettingsDeviations, "\n")
	for _, sub := range []string{
		"terminals_per_warehouse=20",
		"Stock-Level home",
		`pacing="disabled"`,
		`think_time_distribution="compatibility"`,
		"phases.measurement=",
	} {
		if !strings.Contains(joined, sub) {
			t.Fatalf("missing %q in deviations:\n%s", sub, joined)
		}
	}
}

func TestValidate_rejectsManualAssignment(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	p.Raw = map[string]interface{}{
		"workers": []interface{}{
			map[string]interface{}{
				"name":             "worker-a",
				"host":             "load-a",
				"warehouse_ranges": []interface{}{[]interface{}{1, 5}},
			},
		},
	}
	res := validate.Profile(p)
	if res.Valid {
		t.Fatal("expected invalid profile with manual warehouse_ranges")
	}
}

func TestValidate_rejectsObsoleteFields(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	p.Raw["mode"] = "engineering"
	p.Raw["spec"] = map[string]interface{}{"edition": "tpc-c-5.11.0"}
	res := validate.Profile(p)
	if res.Valid {
		t.Fatal("expected invalid profile with obsolete mode/spec fields")
	}
}

func TestValidate_rejectsBadInstanceName(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	p.Workers[0].Name = "Worker-A"
	res := validate.Profile(p)
	if res.Valid {
		t.Fatal("expected invalid instance name")
	}
}

func TestValidate_rejectsBadPasswordEnvNames(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	badNames := []string{
		"TPCC_PASSWORD;touch_x",
		"$(touch_x)",
		"`touch_x`",
		"1TPCC_PASSWORD",
		"TPCC-PASSWORD",
	}
	for _, name := range badNames {
		t.Run(name, func(t *testing.T) {
			p, err := profile.ParseFile(path)
			if err != nil {
				t.Fatal(err)
			}
			p.Database.PasswordEnv = name
			res := validate.Profile(p)
			if res.Valid {
				t.Fatalf("expected invalid password_env %q", name)
			}
			if !strings.Contains(strings.Join(res.Errors, "\n"), "password_env") {
				t.Fatalf("expected password_env error, got %v", res.Errors)
			}
		})
	}
}

func TestValidate_thinkTimeDistribution(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}

	for _, ok := range []string{"", "exponential", "compatibility", "constant"} {
		p.Runtime.ThinkTimeDistribution = ok
		res := validate.Profile(p)
		if !res.Valid {
			t.Fatalf("expected valid think_time_distribution %q, errors: %v", ok, res.Errors)
		}
	}

	p.Runtime.ThinkTimeDistribution = "uniform"
	res := validate.Profile(p)
	if res.Valid {
		t.Fatal("expected invalid think_time_distribution")
	}
}

func TestValidate_ydbAuthSchemes(t *testing.T) {
	path := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	base, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}

	t.Run("anonymous", func(t *testing.T) {
		p := *base
		p.Database = profile.Database{
			DBMS:       "ydb",
			Endpoint:   "grpcs://ydb.example.net:2135",
			Database:   "/Root/tpcc",
			Path:       "tpcc",
			AuthScheme: "anonymous",
		}
		res := validate.Profile(&p)
		if !res.Valid {
			t.Fatalf("expected valid anonymous ydb profile, errors: %v", res.Errors)
		}
	})

	t.Run("login", func(t *testing.T) {
		p := *base
		p.Database = profile.Database{
			DBMS:        "ydb",
			Endpoint:    "grpcs://ydb.example.net:2135",
			Database:    "/Root/tpcc",
			AuthScheme:  "login",
			User:        "root",
			PasswordEnv: "YDB_PASSWORD",
		}
		res := validate.Profile(&p)
		if !res.Valid {
			t.Fatalf("expected valid login ydb profile, errors: %v", res.Errors)
		}
	})

	t.Run("sa_key", func(t *testing.T) {
		p := *base
		p.Database = profile.Database{
			DBMS:       "ydb",
			Endpoint:   "grpcs://ydb.example.net:2135",
			Database:   "/Root/tpcc",
			AuthScheme: "sa_key",
			SaKeyFile:  "./sa-key.json",
			CaFile:     "./ca.pem",
		}
		res := validate.Profile(&p)
		if !res.Valid {
			t.Fatalf("expected valid sa_key ydb profile, errors: %v", res.Errors)
		}
	})

	t.Run("login_requires_user", func(t *testing.T) {
		p := *base
		p.Database = profile.Database{
			DBMS:        "ydb",
			Endpoint:    "localhost:2136",
			Database:    "/local",
			AuthScheme:  "login",
			PasswordEnv: "YDB_PASSWORD",
		}
		res := validate.Profile(&p)
		if res.Valid {
			t.Fatal("expected login without user to fail")
		}
	})

	t.Run("anonymous_rejects_password", func(t *testing.T) {
		p := *base
		p.Database = profile.Database{
			DBMS:        "ydb",
			Endpoint:    "localhost:2136",
			Database:    "/local",
			AuthScheme:  "anonymous",
			PasswordEnv: "YDB_PASSWORD",
		}
		res := validate.Profile(&p)
		if res.Valid {
			t.Fatal("expected anonymous with password_env to fail")
		}
	})

	t.Run("pgsql_rejects_ydb_fields", func(t *testing.T) {
		p := *base
		p.Database.AuthScheme = "login"
		p.Database.User = "root"
		res := validate.Profile(&p)
		if res.Valid {
			t.Fatal("expected pgsql profile with ydb auth fields to fail")
		}
	})
}

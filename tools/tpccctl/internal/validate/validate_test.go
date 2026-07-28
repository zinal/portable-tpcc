package validate_test

import (
	"path/filepath"
	"testing"

	"portable-tpcc/tools/tpccctl/internal/profile"
	"portable-tpcc/tools/tpccctl/internal/validate"
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
				"name":              "worker-a",
				"host":              "load-a",
				"warehouse_ranges": []interface{}{[]interface{}{1, 5}},
			},
		},
	}
	res := validate.Profile(p)
	if res.Valid {
		t.Fatal("expected invalid profile with manual warehouse_ranges")
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

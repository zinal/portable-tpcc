package orchestrator_test

import (
	"os"
	"path/filepath"
	"testing"

	"portable-tpcc/tools/tpccctl/internal/orchestrator"
)

func TestPlan_snapshot(t *testing.T) {
	dir := t.TempDir()
	profileSrc := filepath.Join("..", "..", "testdata", "profile.valid.yaml")
	profilePath := filepath.Join(dir, "profile.yaml")
	data, err := os.ReadFile(profileSrc)
	if err != nil {
		t.Fatal(err)
	}
	// Rewrite paths to temp dir.
	content := string(data)
	content = replaceAll(content, "./dist", filepath.Join(dir, "dist"))
	content = replaceAll(content, "./remote", filepath.Join(dir, "remote"))
	content = replaceAll(content, "./results", filepath.Join(dir, "results"))
	content = replaceAll(content, "./state", filepath.Join(dir, "state"))
	if err := os.WriteFile(profilePath, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(dir, "dist"), 0755); err != nil {
		t.Fatal(err)
	}

	o, err := orchestrator.New(orchestrator.Options{ProfilePath: profilePath})
	if err != nil {
		t.Fatal(err)
	}
	plan, err := o.Plan()
	if err != nil {
		t.Fatal(err)
	}
	if plan.RunID == "" {
		t.Fatal("empty run_id")
	}
	if len(plan.WorkerArgv) != 1 {
		t.Fatalf("worker argv count %d", len(plan.WorkerArgv))
	}
	argv := plan.WorkerArgv["worker-a"]
	if len(argv) != 5 || argv[0] != "worker" {
		t.Fatalf("unexpected argv: %v", argv)
	}
	if plan.RunConfigSHA256 == "" || plan.LoadPlanSHA256 == "" {
		t.Fatal("missing plan hashes")
	}
}

func replaceAll(s, old, new string) string {
	for {
		i := indexOf(s, old)
		if i < 0 {
			return s
		}
		s = s[:i] + new + s[i+len(old):]
	}
}

func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

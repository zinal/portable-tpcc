package deploy_test

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/cleanup"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/deploy"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
)

func TestDeployAndCleanupLocalTwoHosts(t *testing.T) {
	root := t.TempDir()
	host1Root := filepath.Join(root, "host1")
	host2Root := filepath.Join(root, "host2")
	binDir := filepath.Join(root, "bin")
	dataDir := filepath.Join(root, "data")
	if err := os.MkdirAll(binDir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(dataDir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(binDir, "Driver.exe"), []byte("driver-binary"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dataDir, "flat.txt"), []byte("data"), 0644); err != nil {
		t.Fatal(err)
	}

	profileYAML := `
apiVersion: tpcectl/v1
kind: Profile
name: deploy-test
paths:
  local_bin: ` + binDir + `
  local_data: ` + dataDir + `
  local_sql: ` + filepath.Join(root, "sql") + `
  remote_root: /opt/tpce
scale:
  customers: 5000
  active_customers: 5000
  scale_factor: 500
  initial_trade_days: 300
  duration_sec: 60
  client_side: true
db:
  host: localhost
  port: 5432
  name: tpce
  user: tpce
  password_env: TPCE_PGPASSWORD
hosts:
  mid1: { address: 127.0.0.1 }
  mid2: { address: 127.0.0.2 }
deploy:
  artifacts:
    - { src: "{{ local_bin }}/Driver.exe", dst: bin/Driver.exe, mode: "0755" }
    - { src: "{{ local_data }}", dst: data, recursive: true }
bh:
  - { name: bh1, host: mid1, listen: 30000, output: "runs/{{ run_id }}/bh1" }
mee:
  - { name: mee1, host: mid2, listen: 30010, unique_id: 1, output: "runs/{{ run_id }}/mee1" }
standalone_driver:
  enabled: true
  host: mid1
  users: 2
  ce_id_base: 1
  duration_sec: 30
  output: "runs/{{ run_id }}/driver"
`
	path := filepath.Join(root, "profile.yaml")
	if err := os.WriteFile(path, []byte(profileYAML), 0600); err != nil {
		t.Fatal(err)
	}

	p, err := config.Load(path)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	r, err := config.Resolve(p, path, "deploy-test-run")
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	if err := config.Validate(r); err != nil {
		t.Fatalf("validate: %v", err)
	}

	sessions := map[string]*remote.LocalSession{
		"mid1": {Host: "mid1", Root: host1Root},
		"mid2": {Host: "mid2", Root: host2Root},
	}
	dial := func(hostName string, _ *config.ResolvedProfile) (remote.Session, error) {
		s, ok := sessions[hostName]
		if !ok {
			t.Fatalf("unknown host %s", hostName)
		}
		return s, nil
	}

	if err := deploy.Run(r, deploy.Options{}, dial); err != nil {
		t.Fatalf("deploy: %v", err)
	}

	for _, host := range []string{"mid1", "mid2"} {
		s := sessions[host]
		doc, err := s.ReadManifest()
		if err != nil {
			t.Fatalf("%s manifest: %v", host, err)
		}
		if !doc.Complete {
			t.Fatalf("%s manifest not complete", host)
		}
		driverPath := filepath.Join(s.Root, "bin", "Driver.exe")
		if _, err := os.Stat(driverPath); err != nil {
			t.Fatalf("%s missing driver: %v", host, err)
		}
		if _, err := os.Stat(filepath.Join(s.Root, ".tpcectl", "deploy-manifest.json")); err != nil {
			t.Fatalf("%s missing manifest file: %v", host, err)
		}
	}

	if err := cleanup.Run(r, cleanup.Options{Yes: true}, dial); err != nil {
		t.Fatalf("cleanup: %v", err)
	}

	for name, s := range sessions {
		if _, err := os.Stat(filepath.Join(s.Root, "bin", "Driver.exe")); !os.IsNotExist(err) {
			t.Fatalf("%s driver should be removed", name)
		}
		if _, err := os.Stat(filepath.Join(s.Root, ".tpcectl")); err != nil {
			t.Fatalf("%s .tpcectl should remain: %v", name, err)
		}
		if _, err := os.Stat(s.Root); err != nil {
			t.Fatalf("%s remote_root should remain: %v", name, err)
		}
	}
}
package config_test

import (
	"strings"
	"testing"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

func validProfileYAML() string {
	return `
apiVersion: tpcectl/v1
kind: Profile
name: test
paths:
  local_bin: ./bin
  local_data: ./data
  local_sql: ./sql
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
  h1: { address: 127.0.0.1 }
  h2: { address: 127.0.0.2 }
bh:
  - { name: bh1, host: h1, listen: 30000, output: "runs/{{ run_id }}/bh1" }
mee:
  - { name: mee1, host: h1, listen: 30010, unique_id: 1, output: "runs/{{ run_id }}/mee1" }
dm:
  name: dm0
  host: h2
  output: "runs/{{ run_id }}/dm0"
ce:
  - { name: ce1, host: h2, users: 4, ce_id_base: 1, output: "runs/{{ run_id }}/ce1" }
`
}

func resolveYAML(t *testing.T, yaml string) *config.ResolvedProfile {
	t.Helper()
	path := writeTempProfile(t, yaml)
	p, err := config.Load(path)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	r, err := config.Resolve(p, path, "test-run")
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	return r
}

func TestValidateAcceptsMinimalProfile(t *testing.T) {
	r := resolveYAML(t, validProfileYAML())
	if err := config.Validate(r); err != nil {
		t.Fatalf("expected valid profile: %v", err)
	}
}

func TestValidateRejectsDuplicateMEEUniqueID(t *testing.T) {
	yaml := strings.Replace(validProfileYAML(),
		`mee:
  - { name: mee1, host: h1, listen: 30010, unique_id: 1, output: "runs/{{ run_id }}/mee1" }`,
		`mee:
  - { name: mee1, host: h1, listen: 30010, unique_id: 1, output: "runs/{{ run_id }}/mee1" }
  - { name: mee2, host: h2, listen: 30011, unique_id: 1, output: "runs/{{ run_id }}/mee2" }`, 1)
	r := resolveYAML(t, yaml)
	err := config.Validate(r)
	if err == nil {
		t.Fatal("expected validation error")
	}
	if !strings.Contains(err.Error(), "duplicate mee unique_id") {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestValidateRejectsStandaloneWithDM(t *testing.T) {
	yaml := strings.Replace(validProfileYAML(), "dm:", `standalone_driver:
  enabled: true
  host: h2
  users: 4
  ce_id_base: 1
  duration_sec: 60
  output: "runs/{{ run_id }}/driver"
dm:`, 1)
	r := resolveYAML(t, yaml)
	err := config.Validate(r)
	if err == nil {
		t.Fatal("expected validation error")
	}
	if !strings.Contains(err.Error(), "mutually exclusive") {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestValidateRejectsOverlappingCEIntervals(t *testing.T) {
	yaml := strings.Replace(validProfileYAML(),
		`ce:
  - { name: ce1, host: h2, users: 4, ce_id_base: 1, output: "runs/{{ run_id }}/ce1" }`,
		`ce:
  - { name: ce1, host: h2, users: 10, ce_id_base: 1, output: "runs/{{ run_id }}/ce1" }
  - { name: ce2, host: h2, users: 10, ce_id_base: 5, output: "runs/{{ run_id }}/ce2" }`, 1)
	r := resolveYAML(t, yaml)
	err := config.Validate(r)
	if err == nil {
		t.Fatal("expected validation error")
	}
	if !strings.Contains(err.Error(), "overlapping ce_id_base") {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestExpandTemplatesRejectsUnknownPlaceholder(t *testing.T) {
	_, err := config.ExpandTemplates("runs/{{ unknown }}/bh1", map[string]string{"run_id": "x"})
	if err == nil || !strings.Contains(err.Error(), "unknown template placeholder") {
		t.Fatalf("expected unknown placeholder error, got %v", err)
	}
}

func TestValidateBaseTimeEpochAtRun(t *testing.T) {
	now := time.Unix(1_000_000, 0).UTC()
	timeouts := config.TimeoutsConfig{
		ConfigDistribute: 30 * time.Second,
		Ready:            60 * time.Second,
	}
	tooEarly := now.Unix() + 100
	if err := config.ValidateBaseTimeEpochAtRun(tooEarly, now, timeouts); err == nil {
		t.Fatal("expected epoch validation error")
	}
	ok := now.Unix() + int64(timeouts.ConfigDistribute.Seconds()) + 2*int64(timeouts.Ready.Seconds()) + 10
	if err := config.ValidateBaseTimeEpochAtRun(ok, now, timeouts); err != nil {
		t.Fatalf("expected valid epoch: %v", err)
	}
}

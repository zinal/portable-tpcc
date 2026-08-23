package cli

import (
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"portable-tpcc/mind/internal/profile"
	"portable-tpcc/mind/internal/validate"
)

func TestRun_configureWritesValidProfiles(t *testing.T) {
	dir := t.TempDir()
	for _, dbms := range []string{"pgsql", "ydb", "oceanbase"} {
		path := filepath.Join(dir, dbms+".yaml")
		code := Run([]string{"configure", "--profile", path, "--dbms", dbms, "--ssh-user", "tpcc"})
		if code != 0 {
			t.Fatalf("configure %s exit=%d", dbms, code)
		}
		p, err := profile.ParseFile(path)
		if err != nil {
			t.Fatalf("parse %s: %v", dbms, err)
		}
		res := validate.Profile(p)
		if !res.Valid {
			t.Fatalf("validate %s: %v", dbms, res.Errors)
		}
		if p.Database.DBMS != dbms {
			t.Fatalf("dbms=%q, want %q", p.Database.DBMS, dbms)
		}
		if len(p.Loaders) != 1 || p.Loaders[0].Host != "localhost" {
			t.Fatalf("%s loaders=%+v", dbms, p.Loaders)
		}
		if len(p.Workers) != 1 || p.Workers[0].Host != "localhost" {
			t.Fatalf("%s workers=%+v", dbms, p.Workers)
		}
	}
}

func TestRun_configurePositionalPathAndOverrides(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "lab.yaml")
	code := Run([]string{
		"configure", path,
		"--dbms", "pgsql",
		"--name", "lab-pg",
		"--ssh-user", "tpcc",
		"--warehouses", "20",
		"--endpoint", "db.example:5432",
		"--loaders", "localhost,host-a",
		"--workers", "host-b",
		"--after-import",
		"--partitioning", "warehouse_hash",
		"--partition-count", "8",
		"--foreign-keys", "off",
		"--measurement", "30m",
	})
	if code != 0 {
		t.Fatalf("configure exit=%d", code)
	}
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if p.Metadata.Name != "lab-pg" {
		t.Fatalf("name=%q", p.Metadata.Name)
	}
	if p.Scale.Warehouses != 20 {
		t.Fatalf("warehouses=%d", p.Scale.Warehouses)
	}
	if p.Database.Endpoint != "db.example:5432" {
		t.Fatalf("endpoint=%q", p.Database.Endpoint)
	}
	if got := hostList(p.Loaders); got != "localhost,host-a" {
		t.Fatalf("loaders=%q", got)
	}
	if got := hostList(p.Workers); got != "host-b" {
		t.Fatalf("workers=%q", got)
	}
	if !p.Checks.AfterImport {
		t.Fatal("expected after_import")
	}
	if p.Database.Options["partitioning"] != "warehouse_hash" {
		t.Fatalf("partitioning=%v", p.Database.Options["partitioning"])
	}
	if p.Database.Options["partition_count"] != 8 {
		t.Fatalf("partition_count=%v", p.Database.Options["partition_count"])
	}
	if p.Database.Options["foreign_keys"] != "off" {
		t.Fatalf("foreign_keys=%v", p.Database.Options["foreign_keys"])
	}
	if p.Phases.Measurement != "30m" {
		t.Fatalf("measurement=%q", p.Phases.Measurement)
	}
}

func TestRun_configureYdbLoginFromUser(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "ydb.yaml")
	code := Run([]string{
		"configure", "--profile", path, "--dbms", "ydb",
		"--ssh-user", "tpcc",
		"--user", "root",
	})
	if code != 0 {
		t.Fatalf("exit=%d", code)
	}
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if p.Database.AuthScheme != "login" {
		t.Fatalf("auth_scheme=%q", p.Database.AuthScheme)
	}
	if p.Database.User != "root" || p.Database.PasswordEnv != "YDB_PASSWORD" {
		t.Fatalf("user=%q password_env=%q", p.Database.User, p.Database.PasswordEnv)
	}
}

func TestRun_configureRefusesOverwriteWithoutYes(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "p.yaml")
	if code := Run([]string{"configure", "--profile", path, "--dbms", "pgsql", "--ssh-user", "tpcc"}); code != 0 {
		t.Fatalf("first write exit=%d", code)
	}
	stderr := captureStderr(t, func() {
		code := Run([]string{"configure", "--profile", path, "--dbms", "pgsql", "--ssh-user", "tpcc"})
		if code != 2 {
			t.Fatalf("overwrite without --yes exit=%d, want 2", code)
		}
	})
	if !strings.Contains(stderr, "already exists") {
		t.Fatalf("stderr=%q", stderr)
	}
	if code := Run([]string{"configure", "--profile", path, "--dbms", "ydb", "--ssh-user", "tpcc", "--yes"}); code != 0 {
		t.Fatalf("overwrite with --yes exit=%d", code)
	}
	p, err := profile.ParseFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if p.Database.DBMS != "ydb" {
		t.Fatalf("dbms after overwrite=%q", p.Database.DBMS)
	}
}

func TestRun_configureRequiresDBMS(t *testing.T) {
	stderr := captureStderr(t, func() {
		code := Run([]string{"configure", "--profile", "x.yaml"})
		if code != 2 {
			t.Fatalf("exit=%d, want 2", code)
		}
	})
	if !strings.Contains(stderr, "--dbms") {
		t.Fatalf("stderr=%q", stderr)
	}
}

func TestRun_configureRejectsUnknownDBMSAndForeignFlags(t *testing.T) {
	stderr := captureStderr(t, func() {
		code := Run([]string{"configure", "--profile", "x.yaml", "--dbms", "mysql"})
		if code != 2 {
			t.Fatalf("unknown dbms exit=%d", code)
		}
	})
	if !strings.Contains(stderr, "unknown --dbms") {
		t.Fatalf("stderr=%q", stderr)
	}
	stderr = captureStderr(t, func() {
		code := Run([]string{"configure", "--profile", "x.yaml", "--dbms", "ydb", "--partitioning", "none"})
		if code != 2 {
			t.Fatalf("foreign flag exit=%d", code)
		}
	})
	if !strings.Contains(stderr, "only valid for --dbms pgsql") {
		t.Fatalf("stderr=%q", stderr)
	}
}

func TestRun_configureHelp(t *testing.T) {
	old := os.Stdout
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	os.Stdout = w
	code := Run([]string{"configure", "--help"})
	_ = w.Close()
	os.Stdout = old
	if code != 0 {
		t.Fatalf("help=%d", code)
	}
	out, err := io.ReadAll(r)
	if err != nil {
		t.Fatal(err)
	}
	text := string(out)
	for _, want := range []string{"--profile", "--dbms", "--loaders", "--auth-scheme", "--partitions"} {
		if !strings.Contains(text, want) {
			t.Fatalf("configure help missing %q:\n%s", want, text)
		}
	}
}

func TestRun_helpMentionsConfigure(t *testing.T) {
	old := os.Stdout
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	os.Stdout = w
	code := Run([]string{"--help"})
	_ = w.Close()
	os.Stdout = old
	if code != 0 {
		t.Fatalf("help=%d", code)
	}
	out, err := io.ReadAll(r)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(out), "configure") {
		t.Fatalf("global help missing configure:\n%s", out)
	}
}

func hostList(items []profile.NamedHost) string {
	var b []string
	for _, h := range items {
		b = append(b, h.Host)
	}
	return strings.Join(b, ",")
}

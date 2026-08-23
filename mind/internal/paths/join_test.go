package paths

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestJoinRelativeKeepsRemoteRelativeBase(t *testing.T) {
	got, err := JoinRelative("./remote/run-1", "loader/a/result.json")
	if err != nil {
		t.Fatal(err)
	}
	if strings.HasPrefix(got, "/") {
		t.Fatalf("JoinRelative must not Abs against control cwd, got %q", got)
	}
	want := "remote/run-1/loader/a/result.json"
	if got != want {
		t.Fatalf("got %q, want %q", got, want)
	}
}

func TestJoinRelativeHomePrefix(t *testing.T) {
	got, err := JoinRelative("~/portable-tpcc/run-1", "worker/w/result.json")
	if err != nil {
		t.Fatal(err)
	}
	want := "~/portable-tpcc/run-1/worker/w/result.json"
	if got != want {
		t.Fatalf("got %q, want %q", got, want)
	}
}

func TestJoinRelativeRejectsParent(t *testing.T) {
	if _, err := JoinRelative("remote/run-1", "../escape"); err == nil {
		t.Fatal("expected error")
	}
}

func TestResolveUnderHomeUsesAccountHomeNotCwd(t *testing.T) {
	home, err := os.UserHomeDir()
	if err != nil {
		t.Fatal(err)
	}
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chdir(wd) })
	cwd := t.TempDir()
	if err := os.Chdir(cwd); err != nil {
		t.Fatal(err)
	}
	got, err := ResolveUnderHome("portable-tpcc")
	if err != nil {
		t.Fatal(err)
	}
	want := filepath.Join(home, "portable-tpcc")
	if got != want {
		t.Fatalf("ResolveUnderHome(portable-tpcc)=%q, want %q (cwd=%q)", got, want, cwd)
	}
	if strings.HasPrefix(got, cwd) {
		t.Fatalf("resolved under process cwd %q: %q", cwd, got)
	}

	dot, err := ResolveUnderHome("./remote")
	if err != nil {
		t.Fatal(err)
	}
	if dot != filepath.Join(home, "remote") {
		t.Fatalf("ResolveUnderHome(./remote)=%q, want under home", dot)
	}

	abs := filepath.Join(cwd, "abs-root")
	gotAbs, err := ResolveUnderHome(abs)
	if err != nil {
		t.Fatal(err)
	}
	if gotAbs != abs {
		t.Fatalf("absolute path rewritten: %q", gotAbs)
	}

	tilde, err := ResolveUnderHome("~/portable-tpcc")
	if err != nil {
		t.Fatal(err)
	}
	if tilde != want {
		t.Fatalf("~/ form = %q, want %q", tilde, want)
	}
}

func TestRemoteHomeForm(t *testing.T) {
	cases := []struct {
		in, want string
	}{
		{"portable-tpcc", "~/portable-tpcc"},
		{"./remote", "~/remote"},
		{"~/portable-tpcc", "~/portable-tpcc"},
		{"~", "~"},
		{"/var/tmp/tpcc", "/var/tmp/tpcc"},
	}
	for _, tc := range cases {
		if got := RemoteHomeForm(tc.in); got != tc.want {
			t.Fatalf("RemoteHomeForm(%q)=%q, want %q", tc.in, got, tc.want)
		}
	}
}

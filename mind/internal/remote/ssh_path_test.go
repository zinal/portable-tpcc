package remote

import (
	"os"
	"testing"
)

func TestRemotePathExpr(t *testing.T) {
	cases := []struct {
		in, want string
	}{
		{"remote/run-1", "'remote/run-1'"},
		{"/var/tmp/tpcc", "'/var/tmp/tpcc'"},
		{"~/portable-tpcc/run-1", "\"$HOME\"/'portable-tpcc/run-1'"},
		{"~", "\"$HOME\""},
	}
	for _, tc := range cases {
		if got := remotePathExpr(tc.in); got != tc.want {
			t.Fatalf("remotePathExpr(%q)=%q, want %q", tc.in, got, tc.want)
		}
	}
}

func TestChmodCmd(t *testing.T) {
	got := chmodCmd("remote/run-1/tpcc-oceanbase", 0755)
	want := "chmod 0755 'remote/run-1/tpcc-oceanbase'"
	if got != want {
		t.Fatalf("chmodCmd = %q, want %q", got, want)
	}
	got = chmodCmd("~/portable-tpcc/tpcc-oceanbase", os.FileMode(0755))
	want = "chmod 0755 \"$HOME\"/'portable-tpcc/tpcc-oceanbase'"
	if got != want {
		t.Fatalf("chmodCmd home = %q, want %q", got, want)
	}
}

func TestPathUnderWorkDir(t *testing.T) {
	cases := []struct {
		work, path, want string
	}{
		{"remote/run-1", "remote/run-1/tpcc-oceanbase", "tpcc-oceanbase"},
		{"remote/run-1", "remote/run-1/schema/schema-0/stderr.log", "schema/schema-0/stderr.log"},
		{"/home/u/remote/run-1", "/home/u/remote/run-1/tpcc-oceanbase", "tpcc-oceanbase"},
		{"~/portable-tpcc/run-1", "~/portable-tpcc/run-1/tpcc-oceanbase", "tpcc-oceanbase"},
		{"remote/run-1", "/usr/bin/true", "/usr/bin/true"},
		{"remote/run-1", "remote/run-1", "."},
	}
	for _, tc := range cases {
		if got := pathUnderWorkDir(tc.work, tc.path); got != tc.want {
			t.Fatalf("pathUnderWorkDir(%q, %q)=%q, want %q", tc.work, tc.path, got, tc.want)
		}
	}
}

package remote

import "testing"

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

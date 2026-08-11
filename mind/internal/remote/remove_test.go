package remote

import "testing"

func TestRejectUnsafeRemoveAll(t *testing.T) {
	for _, p := range []string{"", "/", ".", "..", "~", "  /  "} {
		if err := rejectUnsafeRemoveAll(p); err == nil {
			t.Fatalf("expected reject for %q", p)
		}
	}
	for _, p := range []string{"/tmp/run-1", "./remote/run-1", "~/ob-work/run-1", "remote/run-1"} {
		if err := rejectUnsafeRemoveAll(p); err != nil {
			t.Fatalf("unexpected reject for %q: %v", p, err)
		}
	}
}

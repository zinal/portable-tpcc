package paths

import (
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

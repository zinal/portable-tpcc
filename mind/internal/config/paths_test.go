package config

import (
	"path/filepath"
	"testing"

	"portable-tpcc/mind/internal/profile"
)

func TestExpandProfilePathsKeepsRemoteRootHostNative(t *testing.T) {
	p := &profile.Profile{
		Paths: profile.Paths{
			LocalArtifacts: "./dist",
			RemoteRoot:     "./remote",
			ResultRoot:     "./results",
			StateDir:       "./state",
		},
		SSH: profile.SSHConfig{
			KnownHosts: "~/.ssh/known_hosts",
		},
	}
	ep, err := ExpandProfilePaths(p)
	if err != nil {
		t.Fatal(err)
	}
	if ep.RemoteRoot != "./remote" {
		t.Fatalf("RemoteRoot = %q, want ./remote (not control-host Abs)", ep.RemoteRoot)
	}
	if filepath.IsAbs(ep.RemoteRoot) {
		t.Fatalf("RemoteRoot unexpectedly absolute: %q", ep.RemoteRoot)
	}
	if ep.KnownHosts == "~/.ssh/known_hosts" {
		t.Fatal("known_hosts should expand ~/ on the control host")
	}
}

func TestExpandProfilePathsKeepsRemoteHomeTilde(t *testing.T) {
	p := &profile.Profile{
		Paths: profile.Paths{
			LocalArtifacts: "./dist",
			RemoteRoot:     "~/portable-tpcc",
			ResultRoot:     "./results",
			StateDir:       "./state",
		},
	}
	ep, err := ExpandProfilePaths(p)
	if err != nil {
		t.Fatal(err)
	}
	if ep.RemoteRoot != "~/portable-tpcc" {
		t.Fatalf("RemoteRoot = %q, want ~/portable-tpcc for remote-home expansion", ep.RemoteRoot)
	}
}

package remote

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/knownhosts"
)

const (
	// Public keys used only as known_hosts fixtures (no private material).
	testEd25519Authorized = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGBAarftlLeoyf+v+nVchEZII/vna2PCV8FaX4vsF5BX"
	testRSAAuthorized     = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQC2txUz3UQQOoj5jtcp4unzuxmgFa/Ai79c8YEhCcl1s6ZurXCMAgPzvKfZoCPLFbY/eRf9S4D/Dh75PaTMWfBXncwhzX9Z4ich156ry+umBzQ9yr//OMvnuNF2ueo+xxe4wdNlQk5OeAF4ZBqkJkGS0YB8W7MPwnRubvl88h/bv6kk3+1j/XiuCQ1ObwVdx37xCi9k8B/LDGbQARkOXLdLb8zxMmJDVa5tcFmXr55Epx3+om0iek0XANOu8qEtcf0eb5W5AzPSc9dBPL+QhBbdoLus071h8pPjryyPt473qKgQ4xilfHKAlrmz/yY2/ViVLjP068gD3cXORI0aFgzB"
)

func writeKnownHosts(t *testing.T, lines ...string) string {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "known_hosts")
	var b []byte
	for _, line := range lines {
		b = append(b, line...)
		b = append(b, '\n')
	}
	if err := os.WriteFile(path, b, 0600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestHostKeyAlgorithmsPrefersKnownEd25519(t *testing.T) {
	path := writeKnownHosts(t, "ob-runner-1 "+testEd25519Authorized)
	cb, err := knownhosts.New(path)
	if err != nil {
		t.Fatal(err)
	}
	got := hostKeyAlgorithms(cb, "ob-runner-1:22")
	want := []string{ssh.KeyAlgoED25519}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("hostKeyAlgorithms = %v, want %v", got, want)
	}
}

func TestHostKeyAlgorithmsRSAIncludesSHA2(t *testing.T) {
	path := writeKnownHosts(t, "ob-runner-1 "+testRSAAuthorized)
	cb, err := knownhosts.New(path)
	if err != nil {
		t.Fatal(err)
	}
	got := hostKeyAlgorithms(cb, "ob-runner-1:22")
	want := []string{ssh.KeyAlgoRSASHA512, ssh.KeyAlgoRSASHA256, ssh.KeyAlgoRSA}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("hostKeyAlgorithms = %v, want %v", got, want)
	}
}

func TestHostKeyAlgorithmsUnknownHost(t *testing.T) {
	path := writeKnownHosts(t, "other-host "+testEd25519Authorized)
	cb, err := knownhosts.New(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := hostKeyAlgorithms(cb, "ob-runner-1:22"); got != nil {
		t.Fatalf("expected nil algorithms for unknown host, got %v", got)
	}
}

func TestHostKeyPolicySetsAlgorithms(t *testing.T) {
	path := writeKnownHosts(t, "ob-runner-1 "+testEd25519Authorized)
	_, algos, err := hostKeyPolicy(DialConfig{KnownHostsPath: path}, "ob-runner-1:22")
	if err != nil {
		t.Fatal(err)
	}
	want := []string{ssh.KeyAlgoED25519}
	if !reflect.DeepEqual(algos, want) {
		t.Fatalf("algos = %v, want %v", algos, want)
	}
}

func TestHostKeyPolicyInsecureHasNoAlgos(t *testing.T) {
	_, algos, err := hostKeyPolicy(DialConfig{InsecureIgnoreHost: true}, "ob-runner-1:22")
	if err != nil {
		t.Fatal(err)
	}
	if algos != nil {
		t.Fatalf("expected nil algos when insecure, got %v", algos)
	}
}

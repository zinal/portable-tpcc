package sshx

import "testing"

func TestBuildAuthMethodsRequiresSource(t *testing.T) {
	_, err := BuildAuthMethods(AuthOptions{UseAgent: false, PrivateKeyPath: ""})
	if err == nil {
		t.Fatal("expected error without auth source")
	}
}

func TestAgentEnabledDefault(t *testing.T) {
	t.Setenv("SSH_AUTH_SOCK", "")
	if AgentEnabled(nil) {
		t.Fatal("expected agent disabled without SSH_AUTH_SOCK")
	}
	t.Setenv("SSH_AUTH_SOCK", "/tmp/fake-agent")
	if !AgentEnabled(nil) {
		t.Fatal("expected agent enabled when SSH_AUTH_SOCK is set")
	}
	falseVal := false
	if AgentEnabled(&falseVal) {
		t.Fatal("expected explicit false to disable agent")
	}
}

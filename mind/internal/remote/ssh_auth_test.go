package remote

import (
	"bytes"
	"crypto"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rand"
	"encoding/pem"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/agent"
)

func TestSSHAuthMethodsCombinesIdentityKeys(t *testing.T) {
	dir := setupSSHHome(t)
	writeIdentity(t, dir, "id_ed25519", mustEd25519(t))
	writeIdentity(t, dir, "id_ecdsa", mustECDSA(t))

	methods, err := sshAuthMethods(DialConfig{})
	if err != nil {
		t.Fatal(err)
	}
	if len(methods) != 1 {
		t.Fatalf("got %d auth methods, want 1 (crypto/ssh only tries one publickey method)", len(methods))
	}
	signers, err := sshSigners(DialConfig{})
	if err != nil {
		t.Fatal(err)
	}
	if len(signers) != 2 {
		t.Fatalf("got %d signers, want 2", len(signers))
	}
}

func TestSSHAuthMethodsNoKeys(t *testing.T) {
	setupSSHHome(t)
	_, err := sshAuthMethods(DialConfig{})
	if err == nil || !strings.Contains(err.Error(), "no SSH authentication methods") {
		t.Fatalf("expected missing-key error, got %v", err)
	}
}

func TestDialSSHTriesSecondIdentityFile(t *testing.T) {
	dir := setupSSHHome(t)
	writeIdentity(t, dir, "id_ed25519", mustEd25519(t))
	accepted := writeIdentity(t, dir, "id_ecdsa", mustECDSA(t))

	addr := startPublicKeySSHServer(t, accepted.PublicKey())
	sess, err := DialSSH("host", addr, DialConfig{
		User:               "tpcc",
		InsecureIgnoreHost: true,
	})
	if err != nil {
		t.Fatalf("dial with second identity: %v", err)
	}
	defer sess.Close()
}

func TestDialSSHUsesFirstIdentityFile(t *testing.T) {
	dir := setupSSHHome(t)
	accepted := writeIdentity(t, dir, "id_ed25519", mustEd25519(t))
	writeIdentity(t, dir, "id_ecdsa", mustECDSA(t))

	addr := startPublicKeySSHServer(t, accepted.PublicKey())
	sess, err := DialSSH("host", addr, DialConfig{
		User:               "tpcc",
		InsecureIgnoreHost: true,
	})
	if err != nil {
		t.Fatalf("dial with first identity: %v", err)
	}
	defer sess.Close()
}

func TestDialSSHRejectsUnauthorizedIdentities(t *testing.T) {
	dir := setupSSHHome(t)
	writeIdentity(t, dir, "id_ed25519", mustEd25519(t))
	writeIdentity(t, dir, "id_ecdsa", mustECDSA(t))
	other, err := ssh.NewSignerFromKey(mustEd25519(t))
	if err != nil {
		t.Fatal(err)
	}

	addr := startPublicKeySSHServer(t, other.PublicKey())
	_, err = DialSSH("host", addr, DialConfig{
		User:               "tpcc",
		InsecureIgnoreHost: true,
	})
	if err == nil || !strings.Contains(err.Error(), "unable to authenticate") {
		t.Fatalf("expected auth failure, got %v", err)
	}
}

func TestDialSSHFallsBackFromAgentToIdentityFile(t *testing.T) {
	dir := setupSSHHome(t)
	agentKey := mustEd25519(t)
	writeIdentity(t, dir, "id_ed25519", agentKey)
	accepted := writeIdentity(t, dir, "id_ecdsa", mustECDSA(t))
	t.Setenv("SSH_AUTH_SOCK", startTestAgent(t, agentKey))

	addr := startPublicKeySSHServer(t, accepted.PublicKey())
	sess, err := DialSSH("host", addr, DialConfig{
		User:               "tpcc",
		UseAgent:           true,
		InsecureIgnoreHost: true,
	})
	if err != nil {
		t.Fatalf("dial after agent key rejected: %v", err)
	}
	defer sess.Close()
}

func setupSSHHome(t *testing.T) string {
	t.Helper()
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("SSH_AUTH_SOCK", "")
	dir := filepath.Join(home, ".ssh")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	return dir
}

func writeIdentity(t *testing.T, dir, name string, key crypto.PrivateKey) ssh.Signer {
	t.Helper()
	block, err := ssh.MarshalPrivateKey(key, "")
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, pem.EncodeToMemory(block), 0600); err != nil {
		t.Fatal(err)
	}
	signer, err := ssh.NewSignerFromKey(key)
	if err != nil {
		t.Fatal(err)
	}
	return signer
}

func mustEd25519(t *testing.T) ed25519.PrivateKey {
	t.Helper()
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return priv
}

func mustECDSA(t *testing.T) *ecdsa.PrivateKey {
	t.Helper()
	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return priv
}

func startPublicKeySSHServer(t *testing.T, accepted ssh.PublicKey) string {
	t.Helper()
	hostKey, err := ssh.NewSignerFromKey(mustEd25519(t))
	if err != nil {
		t.Fatal(err)
	}
	cfg := &ssh.ServerConfig{
		PublicKeyCallback: func(_ ssh.ConnMetadata, key ssh.PublicKey) (*ssh.Permissions, error) {
			if bytes.Equal(key.Marshal(), accepted.Marshal()) {
				return nil, nil
			}
			return nil, fmt.Errorf("unknown public key")
		},
	}
	cfg.AddHostKey(hostKey)

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	go func() {
		for {
			nConn, err := ln.Accept()
			if err != nil {
				return
			}
			go func(nConn net.Conn) {
				defer nConn.Close()
				c, chans, reqs, err := ssh.NewServerConn(nConn, cfg)
				if err != nil {
					return
				}
				go ssh.DiscardRequests(reqs)
				for newCh := range chans {
					_ = newCh.Reject(ssh.UnknownChannelType, "no channels")
				}
				_ = c.Close()
			}(nConn)
		}
	}()
	return ln.Addr().String()
}

func startTestAgent(t *testing.T, keys ...any) string {
	t.Helper()
	sock := filepath.Join(t.TempDir(), "ssh-agent.sock")
	ln, err := net.Listen("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	keyring := agent.NewKeyring()
	for _, k := range keys {
		if err := keyring.Add(agent.AddedKey{PrivateKey: k}); err != nil {
			t.Fatal(err)
		}
	}
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				_ = agent.ServeAgent(keyring, c)
			}(c)
		}
	}()
	return sock
}

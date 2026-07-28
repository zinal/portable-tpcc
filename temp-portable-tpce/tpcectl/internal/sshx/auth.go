package sshx

import (
	"fmt"
	"net"
	"os"

	"golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/agent"
)

// AuthOptions controls SSH authentication method selection (M6 agent support).
type AuthOptions struct {
	PrivateKeyPath string
	UseAgent       bool
}

// AgentEnabled reports whether ssh-agent authentication should be attempted.
func AgentEnabled(explicit *bool) bool {
	if explicit != nil {
		return *explicit
	}
	return os.Getenv("SSH_AUTH_SOCK") != ""
}

// BuildAuthMethods returns public-key auth methods for Dial.
func BuildAuthMethods(opts AuthOptions) ([]ssh.AuthMethod, error) {
	var methods []ssh.AuthMethod

	if opts.UseAgent {
		if sock := os.Getenv("SSH_AUTH_SOCK"); sock != "" {
			conn, err := net.Dial("unix", sock)
			if err != nil {
				return nil, fmt.Errorf("ssh-agent %s: %w", sock, err)
			}
			defer conn.Close()
			signers, err := agent.NewClient(conn).Signers()
			if err != nil {
				return nil, fmt.Errorf("ssh-agent signers: %w", err)
			}
			if len(signers) > 0 {
				methods = append(methods, ssh.PublicKeys(signers...))
			}
		}
	}

	if opts.PrivateKeyPath != "" {
		keyData, err := os.ReadFile(opts.PrivateKeyPath)
		if err != nil {
			return nil, fmt.Errorf("read private key %s: %w", opts.PrivateKeyPath, err)
		}
		signer, err := ssh.ParsePrivateKey(keyData)
		if err != nil {
			return nil, fmt.Errorf("parse private key %s: %w (hint: use ssh-agent for encrypted keys)", opts.PrivateKeyPath, err)
		}
		methods = append(methods, ssh.PublicKeys(signer))
	}

	if len(methods) == 0 {
		return nil, fmt.Errorf("no SSH authentication method available (set SSH_AUTH_SOCK or private_key)")
	}
	return methods, nil
}

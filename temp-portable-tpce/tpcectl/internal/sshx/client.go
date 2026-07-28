package sshx

import (
	"fmt"
	"net"
	"os"
	"path/filepath"
	"time"

	"golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/knownhosts"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

// HostConfig is the effective SSH settings for one logical host.
type HostConfig struct {
	User           string
	Address        string
	Port           int
	PrivateKeyPath string
	KnownHostsPath string
	ConnectTimeout time.Duration
	UseAgent       bool
}

// ResolveHostConfig merges global and per-host SSH settings.
func ResolveHostConfig(p *config.ResolvedProfile, hostName string) (HostConfig, error) {
	if p == nil {
		return HostConfig{}, fmt.Errorf("profile is nil")
	}
	host, ok := p.Hosts[hostName]
	if !ok {
		return HostConfig{}, fmt.Errorf("unknown host %q", hostName)
	}
	addr, err := p.HostAddress(hostName)
	if err != nil {
		return HostConfig{}, err
	}

	cfg := HostConfig{
		User:           p.SSH.User,
		Address:        addr,
		Port:           p.SSH.Port,
		PrivateKeyPath: p.SSH.PrivateKey,
		KnownHostsPath: p.SSH.KnownHosts,
		ConnectTimeout: p.SSH.ConnectTimeout,
		UseAgent:       AgentEnabled(p.SSH.UseAgent),
	}
	if host.SSH != nil {
		if host.SSH.User != "" {
			cfg.User = host.SSH.User
		}
		if host.SSH.Port != 0 {
			cfg.Port = host.SSH.Port
		}
		if host.SSH.PrivateKey != "" {
			cfg.PrivateKeyPath = host.SSH.PrivateKey
		}
		if host.SSH.KnownHosts != "" {
			cfg.KnownHostsPath = host.SSH.KnownHosts
		}
		if host.SSH.ConnectTimeout != 0 {
			cfg.ConnectTimeout = host.SSH.ConnectTimeout
		}
		if host.SSH.UseAgent != nil {
			cfg.UseAgent = *host.SSH.UseAgent
		}
	}
	if cfg.User == "" {
		cfg.User = os.Getenv("USER")
	}
	if cfg.User == "" {
		return HostConfig{}, fmt.Errorf("ssh user is empty for host %q (set ssh.user or $USER)", hostName)
	}
	if cfg.PrivateKeyPath == "" && !cfg.UseAgent {
		cfg.PrivateKeyPath, err = defaultPrivateKey()
		if err != nil {
			return HostConfig{}, fmt.Errorf("host %q: %w", hostName, err)
		}
	}
	if cfg.KnownHostsPath == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			return HostConfig{}, err
		}
		cfg.KnownHostsPath = filepath.Join(home, ".ssh", "known_hosts")
	}
	return cfg, nil
}

func defaultPrivateKey() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	candidates := []string{"id_ed25519", "id_rsa"}
	for _, name := range candidates {
		path := filepath.Join(home, ".ssh", name)
		if _, err := os.Stat(path); err == nil {
			return path, nil
		}
	}
	return "", fmt.Errorf("no default SSH private key found (~/.ssh/id_ed25519 or id_rsa)")
}

// Client wraps an SSH connection to one host.
type Client struct {
	HostName string
	Config   HostConfig
	conn     *ssh.Client
}

// Dial establishes an SSH client connection.
func Dial(hostName string, cfg HostConfig) (*Client, error) {
	auth, err := BuildAuthMethods(AuthOptions{
		PrivateKeyPath: cfg.PrivateKeyPath,
		UseAgent:       cfg.UseAgent,
	})
	if err != nil {
		return nil, err
	}

	hostKeyCallback, err := knownhosts.New(cfg.KnownHostsPath)
	if err != nil {
		return nil, fmt.Errorf("known_hosts %s: %w", cfg.KnownHostsPath, err)
	}

	sshCfg := &ssh.ClientConfig{
		User:            cfg.User,
		Auth:            auth,
		HostKeyCallback: hostKeyCallback,
		Timeout:         cfg.ConnectTimeout,
	}

	addr := net.JoinHostPort(cfg.Address, fmt.Sprintf("%d", cfg.Port))
	conn, err := ssh.Dial("tcp", addr, sshCfg)
	if err != nil {
		return nil, fmt.Errorf("ssh dial %s (%s): %w", hostName, addr, err)
	}
	return &Client{HostName: hostName, Config: cfg, conn: conn}, nil
}

// SSHConn returns the underlying SSH client for SFTP and other subsystems.
func (c *Client) SSHConn() *ssh.Client {
	if c == nil {
		return nil
	}
	return c.conn
}

// Close closes the SSH connection.
func (c *Client) Close() error {
	if c == nil || c.conn == nil {
		return nil
	}
	return c.conn.Close()
}

// Run executes a remote command and returns combined stdout+stderr bytes.
func (c *Client) Run(command string) ([]byte, error) {
	if c == nil || c.conn == nil {
		return nil, fmt.Errorf("ssh client is not connected")
	}
	session, err := c.conn.NewSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()
	return session.CombinedOutput(command)
}

// StartDetached launches a remote command without waiting for long-running children.
func (c *Client) StartDetached(command string) error {
	if c == nil || c.conn == nil {
		return fmt.Errorf("ssh client is not connected")
	}
	session, err := c.conn.NewSession()
	if err != nil {
		return err
	}
	defer session.Close()
	return session.Start(command)
}

// UTCUnixNow returns remote UTC epoch seconds via `date -u +%s`.
func (c *Client) UTCUnixNow() (int64, error) {
	out, err := c.Run("date -u +%s")
	if err != nil {
		return 0, fmt.Errorf("remote date on %s: %w", c.HostName, err)
	}
	var epoch int64
	if _, err := fmt.Sscanf(string(out), "%d", &epoch); err != nil {
		return 0, fmt.Errorf("parse remote date on %s: %q", c.HostName, string(out))
	}
	return epoch, nil
}

// CheckClockSkew compares local and remote UTC clocks (spec-orchestrator §9.4.7).
func CheckClockSkew(local time.Time, remoteEpoch int64, maxSkewSec int64) error {
	diff := local.UTC().Unix() - remoteEpoch
	if diff < 0 {
		diff = -diff
	}
	if diff > maxSkewSec {
		return fmt.Errorf("clock skew %ds exceeds maximum %ds", diff, maxSkewSec)
	}
	return nil
}

// CheckMEEHostsClockSkew validates all MEE hosts before a multi-MEE run.
func CheckMEEHostsClockSkew(p *config.ResolvedProfile) error {
	if p == nil || len(p.MEE) < 2 {
		return nil
	}
	seen := make(map[string]struct{})
	local := time.Now().UTC()
	for _, mee := range p.MEE {
		if _, ok := seen[mee.Host]; ok {
			continue
		}
		seen[mee.Host] = struct{}{}
		cfg, err := ResolveHostConfig(p, mee.Host)
		if err != nil {
			return err
		}
		client, err := Dial(mee.Host, cfg)
		if err != nil {
			return err
		}
		remote, err := client.UTCUnixNow()
		_ = client.Close()
		if err != nil {
			return err
		}
		if err := CheckClockSkew(local, remote, 1); err != nil {
			return fmt.Errorf("host %s: %w", mee.Host, err)
		}
	}
	return nil
}

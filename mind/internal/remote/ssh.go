package remote

import (
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/agent"
	"golang.org/x/crypto/ssh/knownhosts"
)

// SSH implements Session over an SSH connection.
type SSH struct {
	key     string
	address string
	client  *ssh.Client
}

// DialSSH opens an SSH session with known_hosts / insecure host-key policy.
func DialSSH(key, address string, cfg DialConfig) (*SSH, error) {
	if cfg.ConnectTimeout <= 0 {
		cfg.ConnectTimeout = 10 * time.Second
	}
	host := address
	if !strings.Contains(host, ":") {
		host = net.JoinHostPort(host, "22")
	}

	auth, err := sshAuthMethods(cfg)
	if err != nil {
		return nil, err
	}
	hostKeyCallback, algos, err := hostKeyPolicy(cfg, host)
	if err != nil {
		return nil, err
	}

	clientCfg := &ssh.ClientConfig{
		User:              cfg.User,
		Auth:              auth,
		HostKeyCallback:   hostKeyCallback,
		HostKeyAlgorithms: algos,
		Timeout:           cfg.ConnectTimeout,
	}
	client, err := ssh.Dial("tcp", host, clientCfg)
	if err != nil {
		return nil, fmt.Errorf("ssh dial %s: %w", host, err)
	}
	return &SSH{key: key, address: address, client: client}, nil
}

func sshAuthMethods(cfg DialConfig) ([]ssh.AuthMethod, error) {
	var methods []ssh.AuthMethod
	if cfg.UseAgent {
		if sock := os.Getenv("SSH_AUTH_SOCK"); sock != "" {
			conn, err := net.Dial("unix", sock)
			if err == nil {
				ag := agent.NewClient(conn)
				methods = append(methods, ssh.PublicKeysCallback(ag.Signers))
			}
		}
	}
	// Default identity files when agent unavailable / unused.
	for _, name := range []string{"id_ed25519", "id_rsa", "id_ecdsa"} {
		path := filepath.Join(os.Getenv("HOME"), ".ssh", name)
		key, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		signer, err := ssh.ParsePrivateKey(key)
		if err != nil {
			continue
		}
		methods = append(methods, ssh.PublicKeys(signer))
	}
	if len(methods) == 0 {
		return nil, fmt.Errorf("no SSH authentication methods available (agent/keys)")
	}
	return methods, nil
}

// hostKeyPolicy returns the host-key callback and, when known_hosts lists keys
// for host, the preferred HostKeyAlgorithms (OpenSSH-compatible).
//
// golang.org/x/crypto/ssh does not reorder host-key algorithms from known_hosts
// the way OpenSSH does. Without that preference the server may present a key
// type that is not in known_hosts while a different known type exists, which
// surfaces as knownhosts.KeyError "key mismatch" even though `ssh` works.
func hostKeyPolicy(cfg DialConfig, hostWithPort string) (ssh.HostKeyCallback, []string, error) {
	if cfg.InsecureIgnoreHost {
		return ssh.InsecureIgnoreHostKey(), nil, nil
	}
	if cfg.KnownHostsPath == "" {
		return nil, nil, fmt.Errorf("ssh.known_hosts is required unless insecure_ignore_host_key is set")
	}
	cb, err := knownhosts.New(cfg.KnownHostsPath)
	if err != nil {
		return nil, nil, fmt.Errorf("load known_hosts %s: %w", cfg.KnownHostsPath, err)
	}
	return cb, hostKeyAlgorithms(cb, hostWithPort), nil
}

// hostKeyAlgorithms returns algorithms for keys already recorded for host.
// Empty means the host is unknown (or probe failed); leave ClientConfig defaults.
func hostKeyAlgorithms(cb ssh.HostKeyCallback, hostWithPort string) []string {
	// Probe with a dummy key so knownhosts returns KeyError.Want for this host.
	err := cb(hostWithPort, &net.TCPAddr{IP: net.IPv4zero, Port: 22}, probePublicKey{})
	var keyErr *knownhosts.KeyError
	if !errors.As(err, &keyErr) || len(keyErr.Want) == 0 {
		return nil
	}
	seen := make(map[string]struct{}, len(keyErr.Want)+2)
	var algos []string
	add := func(algo string) {
		if algo == "" {
			return
		}
		if _, ok := seen[algo]; ok {
			return
		}
		seen[algo] = struct{}{}
		algos = append(algos, algo)
	}
	for _, k := range keyErr.Want {
		typ := k.Key.Type()
		if typ == ssh.KeyAlgoRSA {
			// RSA known_hosts entries are key format ssh-rsa; prefer SHA-2 sig algos.
			add(ssh.KeyAlgoRSASHA512)
			add(ssh.KeyAlgoRSASHA256)
		}
		add(typ)
	}
	return algos
}

// probePublicKey is a sentinel key used only to discover known_hosts entries.
type probePublicKey struct{}

func (probePublicKey) Type() string                        { return "mind-tpcc-probe" }
func (probePublicKey) Marshal() []byte                     { return []byte("mind-tpcc-probe") }
func (probePublicKey) Verify([]byte, *ssh.Signature) error { return errors.New("probe key") }

func (s *SSH) Key() string     { return s.key }
func (s *SSH) Address() string { return s.address }

func (s *SSH) run(cmd string) (string, string, int, error) {
	session, err := s.client.NewSession()
	if err != nil {
		return "", "", -1, err
	}
	defer session.Close()
	var stdout, stderr strings.Builder
	session.Stdout = &stdout
	session.Stderr = &stderr
	err = session.Run(cmd)
	exit := 0
	if err != nil {
		if ee, ok := err.(*ssh.ExitError); ok {
			exit = ee.ExitStatus()
			err = nil
		} else {
			return stdout.String(), stderr.String(), -1, err
		}
	}
	return stdout.String(), stderr.String(), exit, nil
}

func shellQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'"'"'`) + "'"
}

// ValidEnvName reports whether name is safe as a POSIX-style environment key.
func ValidEnvName(name string) bool {
	if name == "" {
		return false
	}
	for i, r := range name {
		if i == 0 {
			if (r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') || r == '_' {
				continue
			}
			return false
		}
		if (r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '_' {
			continue
		}
		return false
	}
	return true
}

func (s *SSH) Upload(localPath, remotePath string) error {
	data, err := os.ReadFile(localPath)
	if err != nil {
		return err
	}
	return s.WriteFile(remotePath, data)
}

func (s *SSH) Download(remotePath, localPath string) error {
	data, err := s.ReadFile(remotePath)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(localPath), 0755); err != nil {
		return err
	}
	return os.WriteFile(localPath, data, 0644)
}

func (s *SSH) ReadFile(remotePath string) ([]byte, error) {
	session, err := s.client.NewSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()
	out, err := session.Output("cat " + shellQuote(remotePath))
	if err != nil {
		return nil, fmt.Errorf("read %s: %w", remotePath, err)
	}
	return out, nil
}

// Realpath resolves a remote path through symlinks.
func (s *SSH) Realpath(remotePath string) (string, error) {
	cmd := "if command -v realpath >/dev/null 2>&1; then realpath -- " + shellQuote(remotePath) + "; else readlink -f -- " + shellQuote(remotePath) + "; fi"
	stdout, stderr, exit, err := s.run(cmd)
	if err != nil {
		return "", err
	}
	if exit != 0 {
		return "", fmt.Errorf("realpath failed: %s", stderr)
	}
	return strings.TrimSpace(stdout), nil
}

func (s *SSH) WriteFile(remotePath string, data []byte) error {
	if err := s.MkdirAll(filepath.Dir(remotePath)); err != nil {
		return err
	}
	session, err := s.client.NewSession()
	if err != nil {
		return err
	}
	defer session.Close()
	stdin, err := session.StdinPipe()
	if err != nil {
		return err
	}
	cmd := fmt.Sprintf("cat > %s", shellQuote(remotePath))
	if err := session.Start(cmd); err != nil {
		return err
	}
	if _, err := stdin.Write(data); err != nil {
		return err
	}
	stdin.Close()
	return session.Wait()
}

func (s *SSH) MkdirAll(remotePath string) error {
	_, stderr, exit, err := s.run("mkdir -p " + shellQuote(remotePath))
	if err != nil {
		return err
	}
	if exit != 0 {
		return fmt.Errorf("mkdir -p failed: %s", stderr)
	}
	return nil
}

func (s *SSH) Exists(remotePath string) (bool, error) {
	_, _, exit, err := s.run("test -e " + shellQuote(remotePath))
	if err != nil {
		return false, err
	}
	return exit == 0, nil
}

func (s *SSH) Remove(remotePath string) error {
	_, stderr, exit, err := s.run("rm -f " + shellQuote(remotePath))
	if err != nil {
		return err
	}
	if exit != 0 {
		return fmt.Errorf("rm failed: %s", stderr)
	}
	return nil
}

func (s *SSH) StartDetached(workDir, binary string, argv []string, env map[string]string, stdoutPath, stderrPath string) (int, error) {
	for k := range env {
		if !ValidEnvName(k) {
			return 0, fmt.Errorf("invalid environment variable name %q", k)
		}
	}
	if err := s.MkdirAll(workDir); err != nil {
		return 0, err
	}
	if err := s.MkdirAll(filepath.Dir(stdoutPath)); err != nil {
		return 0, err
	}
	var b strings.Builder
	b.WriteString("cd " + shellQuote(workDir) + " && ")
	for k, v := range env {
		b.WriteString(k + "=" + shellQuote(v) + " ")
	}
	b.WriteString("nohup " + shellQuote(binary))
	for _, a := range argv {
		b.WriteString(" " + shellQuote(a))
	}
	b.WriteString(" > " + shellQuote(stdoutPath))
	b.WriteString(" 2> " + shellQuote(stderrPath))
	b.WriteString(" < /dev/null & echo $!")
	stdout, stderr, exit, err := s.run(b.String())
	if err != nil {
		return 0, err
	}
	if exit != 0 {
		return 0, fmt.Errorf("start failed: %s", stderr)
	}
	pidStr := strings.TrimSpace(stdout)
	pid, err := strconv.Atoi(pidStr)
	if err != nil {
		return 0, fmt.Errorf("parse pid %q: %w", pidStr, err)
	}
	return pid, nil
}

func (s *SSH) Signal(pid int, sig string) error {
	sig = strings.ToUpper(sig)
	if !strings.HasPrefix(sig, "SIG") {
		sig = "SIG" + sig
	}
	_, stderr, exit, err := s.run(fmt.Sprintf("kill -s %s %d", sig, pid))
	if err != nil {
		return err
	}
	if exit != 0 {
		return fmt.Errorf("kill failed: %s", stderr)
	}
	return nil
}

func (s *SSH) IsAlive(pid int) (bool, error) {
	_, _, exit, err := s.run(fmt.Sprintf("kill -0 %d", pid))
	if err != nil {
		return false, err
	}
	return exit == 0, nil
}

func (s *SSH) Close() error {
	if s.client == nil {
		return nil
	}
	return s.client.Close()
}

// ensure unused import for io kept for future streaming SCP
var _ = io.EOF

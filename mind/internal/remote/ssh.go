package remote

import (
	"bufio"
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

// remotePathExpr quotes a remote filesystem path for sh -c.
// A leading ~/ is expanded via $HOME on the remote account (shellQuote alone
// would create a literal "~" directory component).
func remotePathExpr(p string) string {
	if p == "~" {
		return "\"$HOME\""
	}
	if strings.HasPrefix(p, "~/") {
		return "\"$HOME\"/" + shellQuote(p[2:])
	}
	return shellQuote(p)
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
	if err := s.WriteFile(remotePath, data); err != nil {
		return err
	}
	// cat > creates files with the remote umask (typically 0644). Match
	// Local.Upload, which always creates with 0755 so worker binaries stay runnable.
	return s.chmod(remotePath, 0755)
}

func chmodCmd(remotePath string, mode os.FileMode) string {
	return fmt.Sprintf("chmod %04o %s", mode.Perm(), remotePathExpr(remotePath))
}

func (s *SSH) chmod(remotePath string, mode os.FileMode) error {
	_, stderr, exit, err := s.run(chmodCmd(remotePath, mode))
	if err != nil {
		return err
	}
	if exit != 0 {
		return fmt.Errorf("chmod failed: %s", stderr)
	}
	return nil
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
	out, err := session.Output("cat " + remotePathExpr(remotePath))
	if err != nil {
		return nil, fmt.Errorf("read %s: %w", remotePath, err)
	}
	return out, nil
}

// Realpath resolves a remote path through symlinks.
func (s *SSH) Realpath(remotePath string) (string, error) {
	expr := remotePathExpr(remotePath)
	cmd := "if command -v realpath >/dev/null 2>&1; then realpath -- " + expr + "; else readlink -f -- " + expr + "; fi"
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
	cmd := fmt.Sprintf("cat > %s", remotePathExpr(remotePath))
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
	_, stderr, exit, err := s.run("mkdir -p " + remotePathExpr(remotePath))
	if err != nil {
		return err
	}
	if exit != 0 {
		return fmt.Errorf("mkdir -p failed: %s", stderr)
	}
	return nil
}

func (s *SSH) Exists(remotePath string) (bool, error) {
	_, _, exit, err := s.run("test -e " + remotePathExpr(remotePath))
	if err != nil {
		return false, err
	}
	return exit == 0, nil
}

func (s *SSH) Remove(remotePath string) error {
	_, stderr, exit, err := s.run("rm -f " + remotePathExpr(remotePath))
	if err != nil {
		return err
	}
	if exit != 0 {
		return fmt.Errorf("rm failed: %s", stderr)
	}
	return nil
}

func (s *SSH) RemoveAll(remotePath string) error {
	if strings.TrimSpace(remotePath) == "" || remotePath == "/" || remotePath == "." {
		return fmt.Errorf("refusing to remove unsafe path %q", remotePath)
	}
	_, stderr, exit, err := s.run("rm -rf " + remotePathExpr(remotePath))
	if err != nil {
		return err
	}
	if exit != 0 {
		return fmt.Errorf("rm -rf failed: %s", stderr)
	}
	return nil
}

// pathUnderWorkDir returns path relative to workDir when path is inside it.
// StartDetached cds into workDir, so binary/log paths must not keep the workDir prefix
// (e.g. workDir=remote/run, binary=remote/run/tpcc-x → tpcc-x).
func pathUnderWorkDir(workDir, path string) string {
	if workDir == "" || path == "" {
		return path
	}
	cleanWork := filepath.Clean(workDir)
	cleanPath := filepath.Clean(path)
	if cleanPath == cleanWork {
		return "."
	}
	sep := string(os.PathSeparator)
	prefix := cleanWork + sep
	if strings.HasPrefix(cleanPath, prefix) {
		return cleanPath[len(prefix):]
	}
	if rel, err := filepath.Rel(cleanWork, cleanPath); err == nil && rel != "." && !strings.HasPrefix(rel, "..") {
		return rel
	}
	return path
}

// shellExecPath makes a relative executable resolve in the current directory.
// Names without '/' are looked up on PATH; after cd into the run dir we need ./tpcc-x.
func shellExecPath(path string) string {
	if path == "" || path == "." || filepath.IsAbs(path) {
		return path
	}
	if strings.HasPrefix(path, "~/") || strings.HasPrefix(path, "./") || strings.HasPrefix(path, "../") {
		return path
	}
	if strings.Contains(path, "/") {
		return path
	}
	return "./" + path
}

// startDetachedShellCmd builds the remote shell line for StartDetached.
// The child is backgrounded under nohup with stdio redirected to files so it
// survives when the SSH channel is closed after the pid is printed.
func startDetachedShellCmd(workDir, binary string, argv []string, env map[string]string, stdoutPath, stderrPath string) (string, error) {
	for k := range env {
		if !ValidEnvName(k) {
			return "", fmt.Errorf("invalid environment variable name %q", k)
		}
	}
	bin := shellExecPath(pathUnderWorkDir(workDir, binary))
	stdoutRel := pathUnderWorkDir(workDir, stdoutPath)
	stderrRel := pathUnderWorkDir(workDir, stderrPath)
	var b strings.Builder
	b.WriteString("cd " + remotePathExpr(workDir) + " && ")
	for k, v := range env {
		b.WriteString(k + "=" + shellQuote(v) + " ")
	}
	b.WriteString("nohup " + remotePathExpr(bin))
	for _, a := range argv {
		b.WriteString(" " + shellQuote(a))
	}
	b.WriteString(" > " + remotePathExpr(stdoutRel))
	b.WriteString(" 2> " + remotePathExpr(stderrRel))
	b.WriteString(" < /dev/null & echo $!")
	return b.String(), nil
}

// readRemotePID reads the first whitespace-trimmed integer line from r.
func readRemotePID(r io.Reader, timeout time.Duration) (int, error) {
	type result struct {
		pid int
		err error
	}
	ch := make(chan result, 1)
	go func() {
		sc := bufio.NewScanner(r)
		if !sc.Scan() {
			if err := sc.Err(); err != nil {
				ch <- result{err: err}
				return
			}
			ch <- result{err: fmt.Errorf("no pid from remote shell")}
			return
		}
		pidStr := strings.TrimSpace(sc.Text())
		pid, err := strconv.Atoi(pidStr)
		if err != nil {
			ch <- result{err: fmt.Errorf("parse pid %q: %w", pidStr, err)}
			return
		}
		ch <- result{pid: pid}
	}()
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case res := <-ch:
		return res.pid, res.err
	case <-timer.C:
		return 0, fmt.Errorf("timeout waiting for remote pid")
	}
}

func (s *SSH) StartDetached(workDir, binary string, argv []string, env map[string]string, stdoutPath, stderrPath string) (int, error) {
	cmd, err := startDetachedShellCmd(workDir, binary, argv, env, stdoutPath, stderrPath)
	if err != nil {
		return 0, err
	}
	if err := s.MkdirAll(workDir); err != nil {
		return 0, err
	}
	if err := s.MkdirAll(filepath.Dir(stdoutPath)); err != nil {
		return 0, err
	}

	session, err := s.client.NewSession()
	if err != nil {
		return 0, err
	}
	// Do not session.Wait()/Run(): OpenSSH keeps the channel open for as long as
	// the background workload remains in the session. That made multi-host load
	// appear sequential — the next StartDetached never began until the first
	// loader exited. Read the pid, then Close the channel; nohup + redirected
	// stdio keep the child running (same intent as Local's Setsid).
	defer session.Close()

	stdout, err := session.StdoutPipe()
	if err != nil {
		return 0, err
	}
	var stderrBuf strings.Builder
	session.Stderr = &stderrBuf

	if err := session.Start(cmd); err != nil {
		return 0, err
	}
	pid, err := readRemotePID(stdout, 15*time.Second)
	_ = session.Close()
	if err != nil {
		if stderrBuf.Len() > 0 {
			return 0, fmt.Errorf("%w: %s", err, strings.TrimSpace(stderrBuf.String()))
		}
		return 0, err
	}
	if pid <= 0 {
		return 0, fmt.Errorf("invalid remote pid %d", pid)
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

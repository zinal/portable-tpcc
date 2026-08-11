package remote

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

// Local implements Session against the local filesystem / processes.
// Remote paths are interpreted relative to LocalRoot when not absolute.
type Local struct {
	key       string
	address   string
	localRoot string
}

// NewLocal creates a local session.
func NewLocal(key, address, localRoot string) (*Local, error) {
	abs, err := filepath.Abs(localRoot)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(abs, 0755); err != nil {
		return nil, err
	}
	return &Local{key: key, address: address, localRoot: abs}, nil
}

func (l *Local) Key() string     { return l.key }
func (l *Local) Address() string { return l.address }

func (l *Local) resolve(remotePath string) string {
	if filepath.IsAbs(remotePath) {
		return remotePath
	}
	return filepath.Join(l.localRoot, remotePath)
}

func (l *Local) Upload(localPath, remotePath string) error {
	dst := l.resolve(remotePath)
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}
	in, err := os.Open(localPath)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0755)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

func (l *Local) Download(remotePath, localPath string) error {
	src := l.resolve(remotePath)
	if err := os.MkdirAll(filepath.Dir(localPath), 0755); err != nil {
		return err
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(localPath)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

func (l *Local) ReadFile(remotePath string) ([]byte, error) {
	return os.ReadFile(l.resolve(remotePath))
}

func (l *Local) WriteFile(remotePath string, data []byte) error {
	return l.WriteFileMode(remotePath, data, 0644)
}

func (l *Local) WriteFileMode(remotePath string, data []byte, mode os.FileMode) error {
	path := l.resolve(remotePath)
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	perm := mode.Perm()
	if perm == 0 {
		perm = 0644
	}
	if err := os.WriteFile(tmp, data, perm); err != nil {
		return err
	}
	if err := os.Chmod(tmp, perm); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, path)
}

func (l *Local) MkdirAll(remotePath string) error {
	return os.MkdirAll(l.resolve(remotePath), 0755)
}

func (l *Local) Exists(remotePath string) (bool, error) {
	_, err := os.Stat(l.resolve(remotePath))
	if err == nil {
		return true, nil
	}
	if os.IsNotExist(err) {
		return false, nil
	}
	return false, err
}

func (l *Local) Remove(remotePath string) error {
	err := os.Remove(l.resolve(remotePath))
	if os.IsNotExist(err) {
		return nil
	}
	return err
}

func (l *Local) RemoveAll(remotePath string) error {
	if err := rejectUnsafeRemoveAll(remotePath); err != nil {
		return err
	}
	err := os.RemoveAll(l.resolve(remotePath))
	if os.IsNotExist(err) {
		return nil
	}
	return err
}

func (l *Local) StartDetached(workDir, binary string, argv []string, env map[string]string, stdoutPath, stderrPath string) (int, error) {
	wd := l.resolve(workDir)
	bin := binary
	if !filepath.IsAbs(bin) {
		cand := filepath.Join(wd, bin)
		if _, err := os.Stat(cand); err == nil {
			bin = cand
		} else {
			bin = l.resolve(binary)
		}
	}
	if err := os.MkdirAll(wd, 0755); err != nil {
		return 0, err
	}
	if err := os.MkdirAll(filepath.Dir(l.resolve(stdoutPath)), 0755); err != nil {
		return 0, err
	}
	stdout, err := os.OpenFile(l.resolve(stdoutPath), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		return 0, err
	}
	stderr, err := os.OpenFile(l.resolve(stderrPath), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		stdout.Close()
		return 0, err
	}

	cmd := exec.Command(bin, argv...)
	cmd.Dir = wd
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	cmd.Env = mergeEnv(os.Environ(), env)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if err := cmd.Start(); err != nil {
		stdout.Close()
		stderr.Close()
		return 0, err
	}
	pid := cmd.Process.Pid
	// Detach: release wait responsibility to the OS; we still close file handles in a goroutine after Wait.
	go func() {
		_ = cmd.Wait()
		stdout.Close()
		stderr.Close()
	}()
	return pid, nil
}

func (l *Local) Signal(pid int, sig string) error {
	proc, err := os.FindProcess(pid)
	if err != nil {
		return err
	}
	switch strings.ToUpper(sig) {
	case "TERM", "SIGTERM":
		return proc.Signal(syscall.SIGTERM)
	case "KILL", "SIGKILL":
		return proc.Signal(syscall.SIGKILL)
	default:
		return fmt.Errorf("unsupported signal %q", sig)
	}
}

func (l *Local) IsAlive(pid int) (bool, error) {
	if pid <= 0 {
		return false, nil
	}
	proc, err := os.FindProcess(pid)
	if err != nil {
		return false, err
	}
	err = proc.Signal(syscall.Signal(0))
	if err == nil {
		return true, nil
	}
	if err == os.ErrProcessDone || err == syscall.ESRCH {
		return false, nil
	}
	// On Linux, ESRCH means gone; other errors may mean permission.
	if errno, ok := err.(syscall.Errno); ok && errno == syscall.ESRCH {
		return false, nil
	}
	return false, nil
}

func (l *Local) Close() error { return nil }

func mergeEnv(base []string, extra map[string]string) []string {
	if len(extra) == 0 {
		return base
	}
	out := make([]string, 0, len(base)+len(extra))
	seen := map[string]bool{}
	for k, v := range extra {
		out = append(out, k+"="+v)
		seen[k] = true
	}
	for _, e := range base {
		eq := strings.IndexByte(e, '=')
		if eq <= 0 {
			out = append(out, e)
			continue
		}
		k := e[:eq]
		if seen[k] {
			continue
		}
		out = append(out, e)
	}
	return out
}

// ParsePID is a helper for process.json pid fields encoded as numbers/strings.
func ParsePID(v interface{}) int {
	switch t := v.(type) {
	case float64:
		return int(t)
	case int:
		return t
	case string:
		n, _ := strconv.Atoi(t)
		return n
	default:
		return 0
	}
}

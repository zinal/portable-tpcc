package process

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/argv"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
)

// TradeCleanupMarker is the DM stdout completion line (spec-orchestrator §9.5).
const TradeCleanupMarker = "Trade-Cleanup transaction completed."

// Start launches one process with nohup (§9.1) and returns PID metadata.
func Start(
	ex remote.Executor,
	profile *config.ResolvedProfile,
	runID string,
	inst argv.InstanceArgv,
) (pid int, pidFile string, err error) {
	if ex == nil || profile == nil {
		return 0, "", fmt.Errorf("executor or profile is nil")
	}

	outputRel := filepath.ToSlash(inst.Output)
	if err := ex.MkdirAll(outputRel, 0755); err != nil {
		return 0, "", err
	}

	pidRel := filepath.ToSlash(filepath.Join(outputRel, "tpcectl.pid"))
	stdoutRel := filepath.ToSlash(filepath.Join(outputRel, "stdout.log"))
	stderrRel := filepath.ToSlash(filepath.Join(outputRel, "stderr.log"))

	root := profile.Paths.RemoteRoot
	cmdLine := inst.Binary + " " + joinArgs(inst.Args)
	stdoutAbs := remote.JoinRemoteAbs(root, stdoutRel)
	stderrAbs := remote.JoinRemoteAbs(root, stderrRel)
	pidAbs := remote.JoinRemoteAbs(root, pidRel)

	inner := fmt.Sprintf(
		`cd %q && nohup %s > %q 2> %q < /dev/null & echo $! > %q`,
		root, cmdLine, stdoutAbs, stderrAbs, pidAbs,
	)

	launch := inner
	if inst.Role == "bh" {
		var err error
		launch, err = WrapPasswordEnv(ex, profile, runID, inst.Name, inner)
		if err != nil {
			return 0, "", err
		}
	}

	if err := ex.StartDetached(launch); err != nil {
		return 0, "", fmt.Errorf("start %s %s: %w", inst.Role, inst.Name, err)
	}

	var pidBytes []byte
	var readErr error
	for i := 0; i < 20; i++ {
		time.Sleep(100 * time.Millisecond)
		pidBytes, readErr = ex.ReadBytes(pidRel)
		if readErr == nil && strings.TrimSpace(string(pidBytes)) != "" {
			break
		}
	}
	if readErr != nil {
		return 0, "", fmt.Errorf("read pid file: %w", readErr)
	}
	pid, err = strconv.Atoi(strings.TrimSpace(string(pidBytes)))
	if err != nil || pid <= 0 {
		return 0, "", fmt.Errorf("invalid pid in %s: %q", pidRel, string(pidBytes))
	}
	return pid, remote.JoinRemoteAbs(root, pidRel), nil
}

func joinArgs(args []string) string {
	parts := make([]string, len(args))
	for i, a := range args {
		parts[i] = shellQuote(a)
	}
	return strings.Join(parts, " ")
}

func shellQuote(s string) string {
	if s == "" {
		return "''"
	}
	if !strings.ContainsAny(s, " \t\n'\"\\$`") {
		return s
	}
	return "'" + strings.ReplaceAll(s, "'", "'\\''") + "'"
}

// WrapPasswordEnv sources db.password_env from a short-lived remote secret file.
func WrapPasswordEnv(ex remote.Executor, profile *config.ResolvedProfile, runID, instance, inner string) (string, error) {
	envName := profile.DB.PasswordEnv
	if envName == "" {
		return inner, nil
	}
	value := os.Getenv(envName)
	if value == "" {
		return "", fmt.Errorf("environment variable %s is required for BH startup", envName)
	}
	secretRel := filepath.ToSlash(filepath.Join(".tpcectl", "secrets", fmt.Sprintf("%s-%s.env", runID, instance)))
	content := []byte(fmt.Sprintf("%s=%s\n", envName, shellQuoteEnv(value)))
	if err := ex.MkdirAll(filepath.Dir(secretRel), 0700); err != nil {
		return "", err
	}
	if err := ex.WriteBytes(secretRel, content, 0600); err != nil {
		return "", err
	}
	root := profile.Paths.RemoteRoot
	return fmt.Sprintf(
		`cd %q && set -a && . %q && rm -f %q && set +a && %s`,
		root, secretRel, secretRel, inner,
	), nil
}

func shellQuoteEnv(v string) string {
	if strings.ContainsAny(v, "'\n") {
		return "'" + strings.ReplaceAll(v, "'", "'\\''") + "'"
	}
	return v
}

// Stop sends SIGTERM, waits grace, then SIGKILL if needed (§9.6).
func Stop(ex remote.Executor, pid int, grace time.Duration) error {
	if pid <= 0 {
		return nil
	}
	script := fmt.Sprintf(
		`pid=%d; if kill -0 "$pid" 2>/dev/null; then kill -TERM "$pid"; fi; `+
			`deadline=$((SECONDS+%d)); while kill -0 "$pid" 2>/dev/null && [ $SECONDS -lt $deadline ]; do sleep 1; done; `+
			`if kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid"; fi; `+
			`if kill -0 "$pid" 2>/dev/null; then exit 1; fi`,
		pid, int(grace.Seconds()))
	_, err := ex.Run(script)
	return err
}

// IsAlive reports whether pid is running on the remote host.
func IsAlive(ex remote.Executor, pid int) (bool, error) {
	if pid <= 0 {
		return false, nil
	}
	_, err := ex.Run(fmt.Sprintf("kill -0 %d", pid))
	return err == nil, nil
}

// WaitTradeCleanup scans DM stdout for the completion marker after startOffset.
func WaitTradeCleanup(ex remote.Executor, stdoutRel string, startOffset int64, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		data, err := ex.ReadBytes(stdoutRel)
		if err != nil {
			time.Sleep(500 * time.Millisecond)
			continue
		}
		if int64(len(data)) < startOffset {
			startOffset = 0
		}
		chunk := data[startOffset:]
		if bytes.Contains(chunk, []byte(TradeCleanupMarker+"\n")) ||
			bytes.Contains(chunk, []byte(TradeCleanupMarker)) {
			return nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return fmt.Errorf("timed out waiting for Trade-Cleanup marker in %s", stdoutRel)
}

// StdoutSize returns current stdout.log size or 0 if missing.
func StdoutSize(ex remote.Executor, stdoutRel string) int64 {
	data, err := ex.ReadBytes(stdoutRel)
	if err != nil {
		return 0
	}
	return int64(len(data))
}

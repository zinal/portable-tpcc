package collect

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/redact"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/runtimeconfig"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/state"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/sshx"
)

// Options controls result collection (spec-orchestrator §12).
type Options struct {
	DryRun  bool
	Verbose bool
	RunID   string
}

// Dialer opens a remote session for a logical host name.
type Dialer func(hostName string, profile *config.ResolvedProfile) (remote.Session, error)

// DefaultDialer uses SSH/SFTP.
func DefaultDialer() Dialer {
	return func(hostName string, profile *config.ResolvedProfile) (remote.Session, error) {
		cfg, err := sshx.ResolveHostConfig(profile, hostName)
		if err != nil {
			return nil, err
		}
		return remote.Dial(hostName, cfg, profile.Paths.RemoteRoot)
	}
}

// Run downloads instance outputs and saves orchestrator metadata.
func Run(
	ctx context.Context,
	profile *config.ResolvedProfile,
	store *state.Store,
	opts Options,
	dial Dialer,
) error {
	if profile == nil || store == nil {
		return fmt.Errorf("profile or store is nil")
	}
	if dial == nil {
		dial = DefaultDialer()
	}

	profileID := state.ProfileID(profile)
	runID, err := store.ResolveRunID(profileID, opts.RunID)
	if err != nil {
		return err
	}
	runState, err := store.LoadRunState(runID)
	if err != nil {
		return err
	}

	dest := profile.Collect.Dest
	if dest == "" {
		return fmt.Errorf("collect.dest is required")
	}
	if err := os.MkdirAll(dest, 0755); err != nil {
		return err
	}

	if opts.DryRun {
		fmt.Printf("dry-run: would collect run_id=%s into %s\n", runID, dest)
		for _, p := range runState.Processes {
			fmt.Printf("  download %s %s from host %s\n", p.Role, p.Name, p.Host)
		}
		return nil
	}

	sessions := make(map[string]remote.Session)
	defer func() {
		for _, sess := range sessions {
			_ = sess.Close()
		}
	}()

	for _, p := range runState.Processes {
		if err := ctx.Err(); err != nil {
			return err
		}
		sess, ok := sessions[p.Host]
		if !ok {
			sess, err = dial(p.Host, profile)
			if err != nil {
				return fmt.Errorf("host %s: %w", p.Host, err)
			}
			sessions[p.Host] = sess
		}
		rel, err := remote.OutputRel(profile.Paths.RemoteRoot, p.Output)
		if err != nil {
			return fmt.Errorf("%s %s: %w", p.Role, p.Name, err)
		}
		localDir := filepath.Join(dest, p.Role, p.Name)
		if opts.Verbose {
			fmt.Printf("collect: %s %s -> %s\n", p.Host, rel, localDir)
		}
		if err := sess.DownloadTree(rel, localDir); err != nil {
			return fmt.Errorf("download %s %s: %w", p.Role, p.Name, err)
		}
	}

	metaDir := filepath.Join(dest, "orchestrator")
	if err := os.MkdirAll(metaDir, 0755); err != nil {
		return err
	}
	if err := saveProfileCopy(profile, filepath.Join(metaDir, "profile.yaml")); err != nil {
		return err
	}
	if err := saveRunStateCopy(store, runID, filepath.Join(metaDir, "run-state.json")); err != nil {
		return err
	}
	if err := saveRunConfig(profile, runID, sessions, filepath.Join(metaDir, "run-config.json")); err != nil {
		return err
	}

	if hook := strings.TrimSpace(profile.Collect.EGenTesterExport); hook != "" {
		if opts.Verbose {
			fmt.Printf("collect: running egen_tester_export hook\n")
		}
		if err := runHook(dest, hook, "EGEN_TESTER_EXPORT"); err != nil {
			return fmt.Errorf("egen_tester_export: %w", err)
		}
	}

	if cmd := strings.TrimSpace(profile.Collect.PostCommand); cmd != "" {
		if opts.Verbose {
			fmt.Printf("collect: running post_command in %s\n", dest)
		}
		if err := runPostCommand(dest, cmd); err != nil {
			return fmt.Errorf("post_command: %w", err)
		}
	}
	return nil
}

func saveProfileCopy(profile *config.ResolvedProfile, path string) error {
	data, err := os.ReadFile(profile.ProfilePath)
	if err != nil {
		return fmt.Errorf("read profile: %w", err)
	}
	return writeFileAtomic(path, redact.ProfileYAML(data), 0644)
}

func saveRunStateCopy(store *state.Store, runID, path string) error {
	data, err := os.ReadFile(store.RunStatePath(runID))
	if err != nil {
		return err
	}
	return writeFileAtomic(path, data, 0600)
}

func saveRunConfig(
	profile *config.ResolvedProfile,
	runID string,
	sessions map[string]remote.Session,
	path string,
) error {
	rel := filepath.ToSlash(filepath.Join("runs", runID, "run-config.json"))
	for _, sess := range sessions {
		if br, ok := sess.(interface {
			ReadBytes(string) ([]byte, error)
		}); ok {
			data, err := br.ReadBytes(rel)
			if err == nil {
				return writeFileAtomic(path, data, 0644)
			}
		}
	}
	for host := range sessions {
		ex, err := remote.DefaultExecutorDialer()(host, profile)
		if err != nil {
			continue
		}
		data, err := ex.ReadBytes(rel)
		_ = ex.Close()
		if err == nil {
			return writeFileAtomic(path, data, 0644)
		}
	}
	_, raw, _, err := runtimeconfig.Build(profile, runtimeconfig.BuildOptions{})
	if err != nil {
		return nil
	}
	return writeFileAtomic(path, raw, 0644)
}

func writeFileAtomic(path string, data []byte, mode os.FileMode) error {
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, mode); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func runPostCommand(dir, cmd string) error {
	return runHook(dir, cmd, "POST_COMMAND")
}

func runHook(dir, cmd, label string) error {
	c := exec.Command("bash", "-c", cmd)
	c.Dir = dir
	c.Env = append(os.Environ(), "TPCECTL_COLLECT_DEST="+dir, "TPCECTL_HOOK="+label)
	out, err := c.CombinedOutput()
	if err != nil {
		msg := redact.Tail(string(out), 20)
		if msg != "" {
			return fmt.Errorf("%w: %s", err, msg)
		}
		return err
	}
	return nil
}

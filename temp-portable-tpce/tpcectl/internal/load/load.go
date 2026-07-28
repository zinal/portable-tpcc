package load

import (
	"context"
	"fmt"
	"path/filepath"
	"sort"
	"strings"
	"sync"

	"golang.org/x/sync/errgroup"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/database"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/process"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/redact"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/schema"
)

// Options controls Loader execution (spec-orchestrator §11.2).
type Options struct {
	DryRun   bool
	Verbose  bool
	OnlyRole string
	RunID    string
}

// Run executes Loader shards in parallel and applies deferred DDL when configured.
func Run(ctx context.Context, profile *config.ResolvedProfile, opts Options, dial remote.ExecutorDialer) error {
	if profile == nil {
		return fmt.Errorf("profile is nil")
	}
	if opts.OnlyRole != "" && opts.OnlyRole != "loader" {
		return nil
	}
	if len(profile.Load.Shards) == 0 {
		return fmt.Errorf("load.shards is empty")
	}
	if dial == nil {
		dial = remote.DefaultExecutorDialer()
	}

	if opts.DryRun {
		for _, shard := range profile.Load.Shards {
			cmd, err := loaderCommand(profile, shard)
			if err != nil {
				return err
			}
			fmt.Printf("dry-run: [%s] %s\n", shard.Host, cmd)
		}
		if schema.IndexesDeferred(profile) {
			if profile.Schema.ApplyIndexes {
				fmt.Println("dry-run: would apply create_indexes.sql after load")
			}
			if profile.Schema.ApplyFKs {
				fmt.Println("dry-run: would apply create_fks.sql after load")
			}
		}
		return nil
	}

	runID := opts.RunID
	if runID == "" {
		runID = profile.EffectiveRunID
	}

	var mu sync.Mutex
	var firstErr error
	recordErr := func(err error) {
		if err == nil {
			return
		}
		mu.Lock()
		defer mu.Unlock()
		if firstErr == nil {
			firstErr = err
		}
	}

	g, gctx := errgroup.WithContext(ctx)
	for _, shard := range profile.Load.Shards {
		shard := shard
		g.Go(func() error {
			select {
			case <-gctx.Done():
				return gctx.Err()
			default:
			}
			mu.Lock()
			err := firstErr
			mu.Unlock()
			if err != nil {
				return err
			}

			if opts.Verbose {
				fmt.Printf("load: starting shard host=%s begin=%d count=%d\n",
					shard.Host, shard.Begin, shard.Count)
			}
			if err := runShard(gctx, dial, profile, runID, shard); err != nil {
				recordErr(err)
				return err
			}
			if opts.Verbose {
				fmt.Printf("load: completed shard host=%s begin=%d count=%d\n",
					shard.Host, shard.Begin, shard.Count)
			}
			return nil
		})
	}
	if err := g.Wait(); err != nil {
		return err
	}

	schemaOpts := schema.Options{Verbose: opts.Verbose}
	return schema.ApplyPostLoad(ctx, profile, schemaOpts)
}

func runShard(
	ctx context.Context,
	dial remote.ExecutorDialer,
	profile *config.ResolvedProfile,
	runID string,
	shard config.LoadShard,
) error {
	select {
	case <-ctx.Done():
		return ctx.Err()
	default:
	}

	ex, err := dial(shard.Host, profile)
	if err != nil {
		return fmt.Errorf("host %s: %w", shard.Host, err)
	}
	defer ex.Close()

	cmd, err := loaderCommand(profile, shard)
	if err != nil {
		return err
	}
	launch, err := process.WrapPasswordEnv(ex, profile, runID, fmt.Sprintf("loader-%s-%d", shard.Host, shard.Begin), cmd)
	if err != nil {
		return fmt.Errorf("host %s: %w", shard.Host, err)
	}

	out, err := ex.Run(launch)
	if err != nil {
		msg := redact.Tail(strings.TrimSpace(string(out)), 20)
		if msg != "" {
			return fmt.Errorf("host %s loader failed: %w: %s", shard.Host, err, msg)
		}
		return fmt.Errorf("host %s loader failed: %w", shard.Host, err)
	}
	return nil
}

// LoaderCommand builds the Loader argv for one shard (spec-orchestrator §9.3).
func LoaderCommand(profile *config.ResolvedProfile, shard config.LoadShard) (string, error) {
	return loaderCommand(profile, shard)
}

func loaderCommand(profile *config.ResolvedProfile, shard config.LoadShard) (string, error) {
	conninfo, err := database.ConnInfo(profile, false)
	if err != nil {
		return "", err
	}
	root := profile.Paths.RemoteRoot
	dataDir := filepath.ToSlash(filepath.Join(root, "data"))
	args := []string{
		"bin/Loader.exe",
		"-l", "CUSTOM",
		"-i", dataDir,
		"-b", fmt.Sprintf("%d", shard.Begin),
		"-c", fmt.Sprintf("%d", shard.Count),
		"-t", fmt.Sprintf("%d", profile.Scale.Customers),
		"-f", fmt.Sprintf("%d", profile.Scale.ScaleFactor),
		"-w", fmt.Sprintf("%d", profile.Scale.InitialTradeDays),
		"-p", conninfo,
	}
	return fmt.Sprintf("cd %q && %s", root, shellJoin(args)), nil
}

func shellJoin(parts []string) string {
	quoted := make([]string, len(parts))
	for i, p := range parts {
		quoted[i] = shellQuote(p)
	}
	return strings.Join(quoted, " ")
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

// ShardHosts returns sorted unique host names from load.shards.
func ShardHosts(profile *config.ResolvedProfile) []string {
	seen := make(map[string]struct{})
	for _, shard := range profile.Load.Shards {
		if shard.Host != "" {
			seen[shard.Host] = struct{}{}
		}
	}
	out := make([]string, 0, len(seen))
	for host := range seen {
		out = append(out, host)
	}
	sort.Strings(out)
	return out
}

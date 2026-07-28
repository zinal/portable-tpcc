package runconfigdist

import (
	"context"
	"fmt"
	"path/filepath"
	"sync"
	"time"

	"golang.org/x/sync/errgroup"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
)

// Distribute uploads byte-identical run-config.json to every runtime host and verifies SHA-256.
func Distribute(
	ctx context.Context,
	profile *config.ResolvedProfile,
	raw []byte,
	expectedSHA string,
	dial remote.ExecutorDialer,
	timeout time.Duration,
) error {
	if profile == nil {
		return fmt.Errorf("profile is nil")
	}
	hosts := profile.RuntimeHosts()
	if len(hosts) == 0 {
		return fmt.Errorf("no runtime hosts")
	}

	rel := runConfigRel(profile.EffectiveRunID)
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	g, ctx := errgroup.WithContext(ctx)
	var mu sync.Mutex
	for _, hostName := range hosts {
		hostName := hostName
		g.Go(func() error {
			if err := ctx.Err(); err != nil {
				return err
			}
			ex, err := dial(hostName, profile)
			if err != nil {
				return err
			}
			defer ex.Close()

			if err := ex.MkdirAll(filepath.Dir(rel), 0755); err != nil {
				return fmt.Errorf("%s: mkdir: %w", hostName, err)
			}
			if err := ex.WriteBytes(rel, raw, 0644); err != nil {
				return fmt.Errorf("%s: write run-config: %w", hostName, err)
			}
			got, err := remote.FileSHA256(ex, rel)
			if err != nil {
				return fmt.Errorf("%s: hash run-config: %w", hostName, err)
			}
			if got != expectedSHA {
				return fmt.Errorf("%s: run-config sha256 mismatch (got %s want %s)", hostName, got, expectedSHA)
			}
			mu.Lock()
			defer mu.Unlock()
			return nil
		})
	}
	return g.Wait()
}

func runConfigRel(runID string) string {
	return filepath.ToSlash(filepath.Join("runs", runID, "run-config.json"))
}

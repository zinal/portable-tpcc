package cleanup

import (
	"fmt"
	"os"
	"sort"
	"strings"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/deploy"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/manifest"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
)

// Options controls cleanup behavior (spec-orchestrator §13).
type Options struct {
	Yes        bool
	DryRun     bool
	Verbose    bool
	DeleteRuns bool
	RunID      string
}

// Run removes artifacts recorded in each host's deployment manifest.
func Run(profile *config.ResolvedProfile, opts Options, dial deploy.Dialer) error {
	if profile == nil {
		return fmt.Errorf("profile is nil")
	}
	if !opts.Yes && !opts.DryRun {
		if !isTerminal(os.Stdin) {
			return fmt.Errorf("non-interactive cleanup requires --yes")
		}
		fmt.Print("Proceed with cleanup? [y/N] ")
		var answer string
		if _, err := fmt.Scanln(&answer); err != nil {
			return fmt.Errorf("cleanup cancelled")
		}
		if answer != "y" && answer != "Y" {
			return fmt.Errorf("cleanup cancelled")
		}
	}
	if dial == nil {
		dial = deploy.DefaultDialer()
	}

	hosts := profile.DeployHosts()
	sort.Strings(hosts)
	for _, hostName := range hosts {
		if err := cleanupHost(hostName, profile, opts, dial); err != nil {
			return fmt.Errorf("host %s: %w", hostName, err)
		}
	}
	return nil
}

func cleanupHost(hostName string, profile *config.ResolvedProfile, opts Options, dial deploy.Dialer) error {
	if opts.DryRun {
		fmt.Printf("[dry-run] %s: cleanup using %s/%s\n", hostName, profile.Paths.RemoteRoot, manifest.RelativePath)
		if opts.DeleteRuns && opts.RunID != "" {
			fmt.Printf("[dry-run] %s: delete runs/%s\n", hostName, opts.RunID)
		}
		return nil
	}

	session, err := dial(hostName, profile)
	if err != nil {
		return err
	}
	defer session.Close()

	doc, err := session.ReadManifest()
	if err != nil {
		return fmt.Errorf("read manifest: %w", err)
	}
	if err := doc.Validate(hostName, profile.Paths.RemoteRoot); err != nil {
		return err
	}

	files, dirs := partitionEntries(doc.Entries)
	for _, entry := range files {
		if opts.Verbose {
			fmt.Printf("%s: remove file %s\n", hostName, entry.Path)
		}
		if err := session.Remove(entry.Path); err != nil && !isMissing(err) {
			return err
		}
	}

	sort.Slice(dirs, func(i, j int) bool {
		return len(dirs[i].Path) > len(dirs[j].Path)
	})
	for _, entry := range dirs {
		if !entry.CreatedByTpcectl {
			continue
		}
		if opts.Verbose {
			fmt.Printf("%s: remove dir %s (if empty)\n", hostName, entry.Path)
		}
		if err := session.RemoveEmptyDir(entry.Path); err != nil && !isMissing(err) {
			if opts.Verbose {
				fmt.Printf("%s: skip dir %s: %v\n", hostName, entry.Path, err)
			}
		}
	}

	if opts.DeleteRuns {
		if opts.RunID == "" {
			return fmt.Errorf("--delete-runs requires --run-id")
		}
		runPath := "runs/" + opts.RunID
		if opts.Verbose {
			fmt.Printf("%s: remove run tree %s\n", hostName, runPath)
		}
		if err := session.RemoveAll(runPath); err != nil && !isMissing(err) {
			return err
		}
	}

	doc.Complete = false
	doc.DeployedAt = time.Now().UTC().Format(time.RFC3339)
	doc.Entries = nil
	return session.WriteManifest(doc)
}

func partitionEntries(entries []manifest.Entry) (files []manifest.Entry, dirs []manifest.Entry) {
	for _, e := range entries {
		switch e.Type {
		case manifest.TypeFile, manifest.TypeSymlink:
			files = append(files, e)
		case manifest.TypeDir:
			if e.Path == ".tpcectl" || e.Path == "runs" {
				continue
			}
			dirs = append(dirs, e)
		}
	}
	return files, dirs
}

func isMissing(err error) bool {
	if err == nil {
		return false
	}
	if os.IsNotExist(err) {
		return true
	}
	msg := err.Error()
	return strings.Contains(msg, "no such file") || strings.Contains(msg, "does not exist")
}

func isTerminal(f *os.File) bool {
	info, err := f.Stat()
	if err != nil {
		return false
	}
	return (info.Mode() & os.ModeCharDevice) != 0
}

var _ remote.Session = (*remote.LocalSession)(nil)

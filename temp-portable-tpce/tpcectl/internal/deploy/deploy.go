package deploy

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/manifest"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/sshx"
)

// Options controls deployment behavior.
type Options struct {
	DryRun  bool
	Verbose bool
	Now     time.Time
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

// Run deploys artifacts to all deploy hosts (spec-orchestrator §8).
func Run(profile *config.ResolvedProfile, opts Options, dial Dialer) error {
	if profile == nil {
		return fmt.Errorf("profile is nil")
	}
	if dial == nil {
		dial = DefaultDialer()
	}
	now := opts.Now
	if now.IsZero() {
		now = time.Now().UTC()
	}

	hosts := profile.DeployHosts()
	sort.Strings(hosts)
	if len(hosts) == 0 {
		return fmt.Errorf("no deploy hosts in profile")
	}

	artifacts, err := prepareArtifacts(profile)
	if err != nil {
		return err
	}

	for _, hostName := range hosts {
		if err := deployHost(hostName, profile, artifacts, opts, dial, now); err != nil {
			return fmt.Errorf("host %s: %w", hostName, err)
		}
	}
	return nil
}

type preparedArtifact struct {
	Src       string
	Dst       string
	Mode      os.FileMode
	Recursive bool
	Optional  bool
}

func prepareArtifacts(profile *config.ResolvedProfile) ([]preparedArtifact, error) {
	if len(profile.Deploy.Artifacts) == 0 {
		return nil, fmt.Errorf("deploy.artifacts is empty")
	}
	out := make([]preparedArtifact, 0, len(profile.Deploy.Artifacts))
	for i, art := range profile.Deploy.Artifacts {
		dst, err := manifest.NormalizeRelativePath(art.Dst)
		if err != nil {
			return nil, fmt.Errorf("deploy.artifacts[%d].dst: %w", i, err)
		}
		mode, err := parseFileMode(art.Mode)
		if err != nil {
			return nil, fmt.Errorf("deploy.artifacts[%d].mode: %w", i, err)
		}
		out = append(out, preparedArtifact{
			Src:       art.Src,
			Dst:       dst,
			Mode:      mode,
			Recursive: art.Recursive,
			Optional:  art.Optional,
		})
	}
	return out, nil
}

func deployHost(hostName string, profile *config.ResolvedProfile, artifacts []preparedArtifact, opts Options, dial Dialer, now time.Time) error {
	if opts.DryRun {
		for _, art := range artifacts {
			fmt.Printf("[dry-run] %s: deploy %s -> %s/%s\n", hostName, art.Src, profile.Paths.RemoteRoot, art.Dst)
		}
		return nil
	}

	session, err := dial(hostName, profile)
	if err != nil {
		return err
	}
	defer session.Close()

	doc := manifest.New(profile.Name, hostName, profile.Paths.RemoteRoot, now)
	if err := session.WriteManifest(doc); err != nil {
		return fmt.Errorf("write initial manifest: %w", err)
	}

	if err := session.EnsureRoot(); err != nil {
		return fmt.Errorf("create remote_root: %w", err)
	}
	if err := session.MkdirAll(".tpcectl", 0700); err != nil {
		return err
	}
	doc.Entries = manifest.UpsertEntry(doc.Entries, manifest.Entry{
		Path: ".tpcectl", Type: manifest.TypeDir, CreatedByTpcectl: true,
	})
	if err := persistManifest(session, doc); err != nil {
		return err
	}

	for _, art := range artifacts {
		if art.Optional {
			if _, err := os.Stat(art.Src); os.IsNotExist(err) {
				if opts.Verbose {
					fmt.Printf("skip optional artifact %s (not found)\n", art.Src)
				}
				continue
			}
		}
		entries, err := deployArtifact(session, art, profile.Deploy.TarUploadEnabled())
		if err != nil {
			return err
		}
		for _, entry := range entries {
			doc.Entries = manifest.UpsertEntry(doc.Entries, entry)
		}
		if err := persistManifest(session, doc); err != nil {
			return err
		}
	}

	doc.Complete = true
	doc.DeployedAt = now.UTC().Format(time.RFC3339)
	return session.WriteManifest(doc)
}

func deployArtifact(session remote.Session, art preparedArtifact, useTar bool) ([]manifest.Entry, error) {
	info, err := os.Stat(art.Src)
	if err != nil {
		return nil, err
	}

	var entries []manifest.Entry
	for _, dir := range parentDirs(art.Dst) {
		if err := session.MkdirAll(dir, 0755); err != nil {
			return nil, err
		}
		entries = append(entries, manifest.Entry{
			Path: dir, Type: manifest.TypeDir, CreatedByTpcectl: true,
		})
	}

	if art.Recursive {
		if !info.IsDir() {
			return nil, fmt.Errorf("%s: recursive artifact must be a directory", art.Src)
		}
		if useTar {
			if err := session.UploadTreeTar(art.Dst, art.Src); err != nil {
				return nil, err
			}
		} else if err := session.UploadTree(art.Dst, art.Src); err != nil {
			return nil, err
		}
		fileEntries, err := manifestTree(art.Dst, art.Src)
		if err != nil {
			return nil, err
		}
		entries = append(entries, fileEntries...)
		return entries, nil
	}

	if info.IsDir() {
		return nil, fmt.Errorf("%s: non-recursive artifact must be a file", art.Src)
	}
	if err := session.UploadFile(art.Dst, art.Src, art.Mode); err != nil {
		return nil, err
	}
	sum, err := fileSHA256(art.Src)
	if err != nil {
		return nil, err
	}
	entries = append(entries, manifest.Entry{
		Path: art.Dst, Type: manifest.TypeFile, SHA256: sum, CreatedByTpcectl: true,
	})
	return entries, nil
}

func manifestTree(dstPrefix, localDir string) ([]manifest.Entry, error) {
	var entries []manifest.Entry
	err := filepath.WalkDir(localDir, func(path string, d os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		rel, err := filepath.Rel(localDir, path)
		if err != nil {
			return err
		}
		if rel == "." {
			return nil
		}
		remoteRel := filepath.ToSlash(filepath.Join(dstPrefix, rel))
		if d.IsDir() {
			entries = append(entries, manifest.Entry{
				Path: remoteRel, Type: manifest.TypeDir, CreatedByTpcectl: true,
			})
			return nil
		}
		sum, err := fileSHA256(path)
		if err != nil {
			return err
		}
		entries = append(entries, manifest.Entry{
			Path: remoteRel, Type: manifest.TypeFile, SHA256: sum, CreatedByTpcectl: true,
		})
		return nil
	})
	return entries, err
}

func parentDirs(rel string) []string {
	parts := strings.Split(rel, "/")
	if len(parts) <= 1 {
		return nil
	}
	var dirs []string
	for i := 1; i < len(parts); i++ {
		dirs = append(dirs, strings.Join(parts[:i], "/"))
	}
	return dirs
}

func persistManifest(session remote.Session, doc *manifest.Document) error {
	return session.WriteManifest(doc)
}

func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func parseFileMode(s string) (os.FileMode, error) {
	if s == "" {
		return 0644, nil
	}
	var mode uint32
	if _, err := fmt.Sscanf(s, "%o", &mode); err != nil {
		return 0, fmt.Errorf("invalid mode %q: %w", s, err)
	}
	return os.FileMode(mode), nil
}

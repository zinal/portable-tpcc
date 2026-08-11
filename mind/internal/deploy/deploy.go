package deploy

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"portable-tpcc/mind/internal/canonical"
	"portable-tpcc/mind/internal/paths"
)

// Manifest is the host-local deploy manifest per specification §8.4.
type Manifest struct {
	SchemaVersion int             `json:"schema_version"`
	Complete      bool            `json:"complete"`
	Files         []ManifestEntry `json:"files"`
	UpdatedAt     string          `json:"updated_at"`
}

type ManifestEntry struct {
	RelativePath string `json:"relative_path"`
	SHA256       string `json:"sha256"`
	Size         int64  `json:"size"`
}

// LocalDeploy copies artifacts to a local target directory (engineering local mode).
type LocalDeploy struct {
	SourceRoot string
	TargetRoot string
}

// DeployManifestPath returns path to deploy-manifest.json.
func DeployManifestPath(root string) string {
	return filepath.Join(root, ".mind-tpcc", "deploy-manifest.json")
}

// Deploy copies local artifacts and updates manifest incrementally.
func (d *LocalDeploy) Deploy(artifactDir string, verify bool) (*Manifest, error) {
	if err := os.MkdirAll(d.TargetRoot, 0755); err != nil {
		return nil, err
	}
	manifestPath := DeployManifestPath(d.TargetRoot)
	manifest, _ := loadManifest(manifestPath)
	if manifest == nil {
		manifest = &Manifest{SchemaVersion: 1, Complete: false, Files: []ManifestEntry{}}
	}
	manifest.Complete = false

	entries, err := collectArtifacts(artifactDir)
	if err != nil {
		return nil, err
	}
	index := map[string]ManifestEntry{}
	for _, e := range manifest.Files {
		index[e.RelativePath] = e
	}
	for _, src := range entries {
		rel, err := filepath.Rel(artifactDir, src)
		if err != nil {
			return nil, err
		}
		dst, err := paths.JoinUnder(d.TargetRoot, rel)
		if err != nil {
			return nil, err
		}
		if err := copyFile(src, dst); err != nil {
			return nil, err
		}
		sha, err := canonical.SHA256File(src)
		if err != nil {
			return nil, err
		}
		info, err := os.Stat(src)
		if err != nil {
			return nil, err
		}
		if verify {
			got, err := canonical.SHA256File(dst)
			if err != nil {
				return nil, err
			}
			if got != sha {
				return nil, fmt.Errorf("hash mismatch for %s", rel)
			}
		}
		index[rel] = ManifestEntry{
			RelativePath: rel,
			SHA256:       sha,
			Size:         info.Size(),
		}
	}
	manifest.Files = make([]ManifestEntry, 0, len(index))
	for _, e := range index {
		manifest.Files = append(manifest.Files, e)
	}
	manifest.Complete = true
	if err := writeManifest(manifestPath, manifest); err != nil {
		return nil, err
	}
	return manifest, nil
}

// Cleanup removes only manifest-owned paths.
// A missing deploy-manifest.json is a no-op success: pure SSH deploys do not
// write a control-host manifest, and re-running cleanup after a successful
// remove must not fail.
func Cleanup(root string, yes bool) error {
	if !yes {
		return fmt.Errorf("cleanup requires --yes in non-interactive mode")
	}
	manifestPath := DeployManifestPath(root)
	manifest, err := loadManifest(manifestPath)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	if !manifest.Complete {
		return fmt.Errorf("deploy manifest is not complete; refusing cleanup")
	}
	for _, e := range manifest.Files {
		target, err := paths.JoinUnder(root, e.RelativePath)
		if err != nil {
			return err
		}
		if err := os.Remove(target); err != nil && !os.IsNotExist(err) {
			return err
		}
	}
	if err := os.Remove(manifestPath); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

func collectArtifacts(dir string) ([]string, error) {
	var out []string
	err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if !info.IsDir() {
			out = append(out, path)
		}
		return nil
	})
	return out, err
}

func copyFile(src, dst string) error {
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

func loadManifest(path string) (*Manifest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var m Manifest
	if err := json.Unmarshal(data, &m); err != nil {
		return nil, err
	}
	return &m, nil
}

func writeManifest(path string, m *Manifest) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

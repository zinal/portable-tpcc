package collect

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"portable-tpcc/tpccctl/internal/canonical"
	"portable-tpcc/tpccctl/internal/paths"
)

// ArtifactManifest is published by each process before collection.
type ArtifactManifest struct {
	SchemaVersion int                    `json:"schema_version"`
	Instance      string                 `json:"instance"`
	InstanceNonce string                 `json:"instance_nonce"`
	Finalized     bool                   `json:"finalized"`
	ExitStatus    int                    `json:"exit_status"`
	Payloads      []ArtifactPayloadEntry `json:"payloads"`
}

type ArtifactPayloadEntry struct {
	Path   string `json:"path"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

// CollectionManifest covers all collected artifacts on the control host.
type CollectionManifest struct {
	SchemaVersion int                    `json:"schema_version"`
	RunID         string                 `json:"run_id"`
	SHA256        string                 `json:"sha256"`
	Processes     []ArtifactManifest     `json:"processes"`
	ControlFiles  []ArtifactPayloadEntry `json:"control_files"`
}

// Collector copies and verifies remote/local instance artifacts.
type Collector struct {
	ResultRoot string
}

// CollectInstance copies instance artifacts from source to results/raw.
func (c *Collector) CollectInstance(runID, role, instance, sourceDir string) error {
	destTmp := filepath.Join(c.ResultRoot, runID, "raw", role, instance, ".tmp")
	destFinal := filepath.Join(c.ResultRoot, runID, "raw", role, instance)
	if err := os.RemoveAll(destTmp); err != nil {
		return err
	}
	manifestPath := filepath.Join(sourceDir, "artifact-manifest.json")
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		return fmt.Errorf("read artifact-manifest: %w", err)
	}
	var manifest ArtifactManifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return err
	}
	if !manifest.Finalized {
		return fmt.Errorf("artifact manifest for %s not finalized", instance)
	}
	if err := os.MkdirAll(destTmp, 0755); err != nil {
		return err
	}
	for _, p := range manifest.Payloads {
		src := filepath.Join(sourceDir, p.Path)
		dst, err := paths.JoinUnder(destTmp, p.Path)
		if err != nil {
			return err
		}
		if err := copyFile(src, dst); err != nil {
			return err
		}
		got, err := canonical.SHA256File(dst)
		if err != nil {
			return err
		}
		if got != p.SHA256 {
			return fmt.Errorf("tampered artifact %s: hash mismatch", p.Path)
		}
	}
	if err := os.RemoveAll(destFinal); err != nil && !os.IsNotExist(err) {
		return err
	}
	return os.Rename(destTmp, destFinal)
}

// WriteCollectionManifest atomically writes collection-manifest.json.
func (c *Collector) WriteCollectionManifest(runID string, processes []ArtifactManifest, controlFiles []ArtifactPayloadEntry) error {
	manifest := CollectionManifest{
		SchemaVersion: 1,
		RunID:         runID,
		Processes:     processes,
		ControlFiles:  controlFiles,
	}
	sha, err := canonical.SHA256Any(manifest)
	if err != nil {
		return err
	}
	manifest.SHA256 = sha
	dir := filepath.Join(c.ResultRoot, runID)
	data, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return err
	}
	tmp := filepath.Join(dir, "collection-manifest.json.tmp")
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, filepath.Join(dir, "collection-manifest.json"))
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

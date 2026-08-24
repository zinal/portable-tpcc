package collect

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"portable-tpcc/mind/internal/canonical"
	"portable-tpcc/mind/internal/paths"
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

var allowedArtifactPayloads = map[string]bool{
	"result.json":  true,
	"ready.json":   true,
	"process.json": true,
	"stdout.log":   true,
	"stderr.log":   true,
}

var stdioArtifactPayloads = map[string]bool{
	"stdout.log": true,
	"stderr.log": true,
}

// ValidateArtifactPayloadPath rejects payload names outside the worker artifact set.
func ValidateArtifactPayloadPath(path string) error {
	if !allowedArtifactPayloads[path] {
		return fmt.Errorf("unsupported artifact payload path %q", path)
	}
	return nil
}

// CollectionManifestFileName is written under results/<run_id>/ after a
// successful collect. Standalone consolidate uses its presence to decide
// whether collect still needs to run.
const CollectionManifestFileName = "collection-manifest.json"

// CollectionManifest covers all collected artifacts on the control host.
type CollectionManifest struct {
	SchemaVersion int                    `json:"schema_version"`
	RunID         string                 `json:"run_id"`
	SHA256        string                 `json:"sha256"`
	Processes     []ArtifactManifest     `json:"processes"`
	ControlFiles  []ArtifactPayloadEntry `json:"control_files"`
}

// CollectionManifestPath is results/<run_id>/collection-manifest.json.
func CollectionManifestPath(resultRoot, runID string) string {
	return filepath.Join(resultRoot, runID, CollectionManifestFileName)
}

// HasCollectionManifest reports whether collect completed for this run.
func HasCollectionManifest(resultRoot, runID string) bool {
	st, err := os.Stat(CollectionManifestPath(resultRoot, runID))
	return err == nil && !st.IsDir()
}

// Collector copies and verifies remote/local instance artifacts.
type Collector struct {
	ResultRoot string
}

// CollectInstance copies instance artifacts from source to results/raw.
func (c *Collector) CollectInstance(runID, role, instance, sourceDir string) error {
	destFinal := filepath.Join(c.ResultRoot, runID, "raw", role, instance)
	// destTmp must be a sibling of destFinal. Nesting it at destFinal/.tmp
	// makes RemoveAll(destFinal) delete the staged files before rename.
	destTmp := destFinal + ".tmp"
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
		src, err := paths.JoinUnder(sourceDir, p.Path)
		if err != nil {
			return err
		}
		if err := ValidateArtifactPayloadPath(p.Path); err != nil {
			return err
		}
		src, err = paths.ResolveUnder(sourceDir, p.Path)
		if err != nil {
			return err
		}
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
		if got == p.SHA256 {
			continue
		}
		if stdioArtifactPayloads[p.Path] {
			if err := verifyStdioPrefixHash(dst, p); err == nil {
				continue
			}
		}
		st, statErr := os.Stat(dst)
		size := int64(-1)
		if statErr == nil {
			size = st.Size()
		}
		return fmt.Errorf("tampered artifact %s: hash mismatch (manifest sha256=%s size=%d, file sha256=%s size=%d)",
			p.Path, p.SHA256, p.Size, got, size)
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
	tmp := filepath.Join(dir, CollectionManifestFileName+".tmp")
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, CollectionManifestPath(c.ResultRoot, runID))
}

// verifyStdioPrefixHash accepts stdout.log / stderr.log that grew after the
// binary hashed them. Destructor and threaded-log output can append after
// artifact-manifest.json is written; the hashed prefix must still match.
func verifyStdioPrefixHash(path string, p ArtifactPayloadEntry) error {
	if p.Size < 0 {
		return fmt.Errorf("negative payload size")
	}
	st, err := os.Stat(path)
	if err != nil {
		return err
	}
	if st.Size() < p.Size {
		return fmt.Errorf("stdio artifact truncated")
	}
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	h := sha256.New()
	if p.Size > 0 {
		n, err := io.CopyN(h, f, p.Size)
		if err != nil {
			return err
		}
		if n != p.Size {
			return fmt.Errorf("short stdio prefix")
		}
	}
	got := hex.EncodeToString(h.Sum(nil))
	if got != p.SHA256 {
		return fmt.Errorf("stdio prefix hash mismatch")
	}
	return nil
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

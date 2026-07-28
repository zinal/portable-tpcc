package manifest

import (
	"encoding/json"
	"fmt"
	"path"
	"strings"
	"time"
)

const SchemaVersion = 1

const RelativePath = ".tpcectl/deploy-manifest.json"

// EntryType is a manifest entry kind.
type EntryType string

const (
	TypeFile    EntryType = "file"
	TypeDir     EntryType = "dir"
	TypeSymlink EntryType = "symlink"
)

// Entry records one deployed path relative to remote_root.
type Entry struct {
	Path             string    `json:"path"`
	Type             EntryType `json:"type"`
	SHA256           string    `json:"sha256,omitempty"`
	CreatedByTpcectl bool      `json:"created_by_tpcectl"`
}

// Document is the host-local deployment journal (spec-orchestrator §14.1).
type Document struct {
	SchemaVersion int       `json:"schema_version"`
	ProfileName   string    `json:"profile_name"`
	Host          string    `json:"host"`
	RemoteRoot    string    `json:"remote_root"`
	DeployedAt    string    `json:"deployed_at"`
	Complete      bool      `json:"complete"`
	Entries       []Entry   `json:"entries"`
}

// New creates an incomplete manifest before the first remote mutation.
func New(profileName, host, remoteRoot string, now time.Time) *Document {
	return &Document{
		SchemaVersion: SchemaVersion,
		ProfileName:   profileName,
		Host:          host,
		RemoteRoot:    remoteRoot,
		DeployedAt:    now.UTC().Format(time.RFC3339),
		Complete:      false,
		Entries:       nil,
	}
}

// Validate checks manifest metadata before cleanup.
func (d *Document) Validate(host, remoteRoot string) error {
	if d == nil {
		return fmt.Errorf("manifest is nil")
	}
	if d.SchemaVersion != SchemaVersion {
		return fmt.Errorf("unsupported manifest schema_version %d", d.SchemaVersion)
	}
	if d.Host != host {
		return fmt.Errorf("manifest host %q does not match profile host %q", d.Host, host)
	}
	if d.RemoteRoot != remoteRoot {
		return fmt.Errorf("manifest remote_root %q does not match profile %q", d.RemoteRoot, remoteRoot)
	}
	for _, e := range d.Entries {
		if err := ValidateRelativePath(e.Path); err != nil {
			return fmt.Errorf("entry %q: %w", e.Path, err)
		}
	}
	return nil
}

// UpsertEntry adds or replaces an entry and returns the updated slice.
func UpsertEntry(entries []Entry, entry Entry) []Entry {
	if err := ValidateRelativePath(entry.Path); err != nil {
		return entries
	}
	for i, e := range entries {
		if e.Path == entry.Path {
			entries[i] = entry
			return entries
		}
	}
	return append(entries, entry)
}

// ValidateRelativePath rejects absolute paths and path traversal.
func ValidateRelativePath(p string) error {
	p = path.Clean(strings.ReplaceAll(p, "\\", "/"))
	if p == "." || p == "" {
		return fmt.Errorf("path must not be empty")
	}
	if strings.HasPrefix(p, "/") || path.IsAbs(p) {
		return fmt.Errorf("absolute paths are prohibited: %s", p)
	}
	if p == ".." || strings.HasPrefix(p, "../") || strings.Contains(p, "/../") {
		return fmt.Errorf("path traversal is prohibited: %s", p)
	}
	return nil
}

// NormalizeRelativePath cleans a profile dst path for manifest storage.
func NormalizeRelativePath(p string) (string, error) {
	p = strings.TrimSpace(p)
	p = strings.ReplaceAll(p, "\\", "/")
	p = path.Clean(p)
	if err := ValidateRelativePath(p); err != nil {
		return "", err
	}
	return p, nil
}

// Marshal serializes the manifest deterministically.
func Marshal(d *Document) ([]byte, error) {
	if d == nil {
		return nil, fmt.Errorf("manifest is nil")
	}
	data, err := json.MarshalIndent(d, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(data, '\n'), nil
}

// Unmarshal parses manifest JSON.
func Unmarshal(data []byte) (*Document, error) {
	var d Document
	if err := json.Unmarshal(data, &d); err != nil {
		return nil, err
	}
	return &d, nil
}

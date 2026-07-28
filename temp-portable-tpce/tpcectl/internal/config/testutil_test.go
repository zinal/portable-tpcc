package config_test

import (
	"os"
	"path/filepath"
	"testing"
)

func writeTempProfile(t *testing.T, content string) string {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "profile.yaml")
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatalf("write profile: %v", err)
	}
	return path
}

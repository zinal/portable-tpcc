package remote

import (
	"fmt"
	"path/filepath"
	"strings"
)

// rejectUnsafeRemoveAll blocks recursive deletes of empty/root-like paths.
func rejectUnsafeRemoveAll(remotePath string) error {
	p := strings.TrimSpace(remotePath)
	if p == "" {
		return fmt.Errorf("refusing RemoveAll of empty path")
	}
	if p == "/" || p == "." || p == ".." || p == "~" {
		return fmt.Errorf("refusing RemoveAll of %q", p)
	}
	clean := filepath.Clean(p)
	if clean == "/" || clean == "." || clean == ".." {
		return fmt.Errorf("refusing RemoveAll of %q", p)
	}
	return nil
}

package paths

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// ExpandHome expands a leading ~/ in path using the current user's home directory.
func ExpandHome(path string) (string, error) {
	if !strings.HasPrefix(path, "~/") {
		return path, nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, path[2:]), nil
}

// Normalize resolves symlinks and rejects escapes outside permitted roots.
func Normalize(path string, permittedRoots []string) (string, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	clean := filepath.Clean(abs)
	if strings.Contains(clean, "..") {
		return "", fmt.Errorf("path %q contains parent traversal", path)
	}
	resolved, err := filepath.EvalSymlinks(clean)
	if err != nil {
		// File may not exist yet; use clean path.
		resolved = clean
	}
	for _, root := range permittedRoots {
		rootAbs, err := filepath.Abs(root)
		if err != nil {
			continue
		}
		rootClean := filepath.Clean(rootAbs)
		if resolved == rootClean || strings.HasPrefix(resolved, rootClean+string(os.PathSeparator)) {
			return resolved, nil
		}
	}
	return "", fmt.Errorf("path %q escapes permitted roots", path)
}

// JoinUnder joins elem under base and validates the result stays under base.
func JoinUnder(base, elem string) (string, error) {
	if filepath.IsAbs(elem) {
		return "", fmt.Errorf("path %q must be relative", elem)
	}
	for _, part := range strings.FieldsFunc(elem, func(r rune) bool { return r == '/' || r == '\\' }) {
		if part == ".." {
			return "", fmt.Errorf("path %q contains parent traversal", elem)
		}
	}
	joined := filepath.Join(base, elem)
	baseAbs, err := filepath.Abs(base)
	if err != nil {
		return "", err
	}
	joinedAbs, err := filepath.Abs(joined)
	if err != nil {
		return "", err
	}
	baseClean := filepath.Clean(baseAbs)
	joinedClean := filepath.Clean(joinedAbs)
	if joinedClean != baseClean && !strings.HasPrefix(joinedClean, baseClean+string(os.PathSeparator)) {
		return "", fmt.Errorf("joined path %q escapes base %q", joined, base)
	}
	return joinedClean, nil
}

// ResolveUnder joins elem under base, resolves symlinks, and validates the
// resolved target still stays under the resolved base.
func ResolveUnder(base, elem string) (string, error) {
	joined, err := JoinUnder(base, elem)
	if err != nil {
		return "", err
	}
	baseAbs, err := filepath.Abs(base)
	if err != nil {
		return "", err
	}
	baseResolved, err := filepath.EvalSymlinks(baseAbs)
	if err != nil {
		return "", err
	}
	targetResolved, err := filepath.EvalSymlinks(joined)
	if err != nil {
		return "", err
	}
	baseClean := filepath.Clean(baseResolved)
	targetClean := filepath.Clean(targetResolved)
	if targetClean != baseClean && !strings.HasPrefix(targetClean, baseClean+string(os.PathSeparator)) {
		return "", fmt.Errorf("resolved path %q escapes base %q", targetClean, baseClean)
	}
	return targetClean, nil
}

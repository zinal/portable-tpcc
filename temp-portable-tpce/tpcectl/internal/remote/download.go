package remote

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/pkg/sftp"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/manifest"
)

func (s *sftpSession) DownloadTree(relPrefix, localDir string) error {
	prefix, err := manifest.NormalizeRelativePath(relPrefix)
	if err != nil {
		return err
	}
	remoteAbs, err := s.abs(prefix)
	if err != nil {
		return err
	}
	return downloadTreeSFTP(s.sftp, remoteAbs, localDir)
}

func downloadTreeSFTP(client *sftp.Client, remoteDir, localDir string) error {
	entries, err := client.ReadDir(remoteDir)
	if err != nil {
		return fmt.Errorf("read remote dir %s: %w", remoteDir, err)
	}
	if err := os.MkdirAll(localDir, 0755); err != nil {
		return err
	}
	for _, entry := range entries {
		name := entry.Name()
		remotePath := filepath.Join(remoteDir, name)
		localPath := filepath.Join(localDir, name)
		if entry.IsDir() {
			if err := downloadTreeSFTP(client, remotePath, localPath); err != nil {
				return err
			}
			continue
		}
		if err := downloadFileSFTP(client, remotePath, localPath); err != nil {
			return err
		}
	}
	return nil
}

func downloadFileSFTP(client *sftp.Client, remotePath, localPath string) error {
	src, err := client.Open(remotePath)
	if err != nil {
		return fmt.Errorf("open remote %s: %w", remotePath, err)
	}
	defer src.Close()
	if err := os.MkdirAll(filepath.Dir(localPath), 0755); err != nil {
		return err
	}
	dst, err := os.Create(localPath)
	if err != nil {
		return err
	}
	if _, err := io.Copy(dst, src); err != nil {
		_ = dst.Close()
		return err
	}
	return dst.Close()
}

// OutputRel converts an absolute remote output path to a path relative to remote_root.
func OutputRel(remoteRoot, outputAbs string) (string, error) {
	root := filepath.Clean(remoteRoot)
	out := filepath.Clean(outputAbs)
	rel, err := filepath.Rel(root, out)
	if err != nil {
		return "", err
	}
	if rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return "", fmt.Errorf("output %q is not under remote_root %q", outputAbs, remoteRoot)
	}
	return manifest.NormalizeRelativePath(filepath.ToSlash(rel))
}

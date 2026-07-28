package remote

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/manifest"
)

// LocalSession implements Session on a local directory (tests).
type LocalSession struct {
	Host       string
	Root       string
}

func (s *LocalSession) HostName() string   { return s.Host }
func (s *LocalSession) RemoteRoot() string { return s.Root }

func (s *LocalSession) Close() error { return nil }

func (s *LocalSession) EnsureRoot() error {
	return os.MkdirAll(s.Root, 0755)
}

func (s *LocalSession) Run(cmd string) ([]byte, error) {
	c := exec.Command("bash", "-c", cmd)
	c.Dir = s.Root
	return c.CombinedOutput()
}

func (s *LocalSession) StartDetached(cmd string) error {
	c := exec.Command("bash", "-c", cmd)
	c.Dir = s.Root
	done := make(chan error, 1)
	go func() { done <- c.Run() }()
	select {
	case err := <-done:
		return err
	case <-time.After(3 * time.Second):
		return nil
	}
}

func (s *LocalSession) WriteBytes(rel string, data []byte, perm os.FileMode) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(abs), 0755); err != nil {
		return err
	}
	tmp := abs + ".tmp"
	if err := os.WriteFile(tmp, data, perm); err != nil {
		return err
	}
	return os.Rename(tmp, abs)
}

func (s *LocalSession) ReadBytes(rel string) ([]byte, error) {
	abs, err := s.abs(rel)
	if err != nil {
		return nil, err
	}
	return os.ReadFile(abs)
}

func (s *LocalSession) abs(rel string) (string, error) {
	norm, err := manifest.NormalizeRelativePath(rel)
	if err != nil {
		return "", err
	}
	return filepath.Join(s.Root, filepath.FromSlash(norm)), nil
}

func (s *LocalSession) MkdirAll(rel string, perm os.FileMode) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	return os.MkdirAll(abs, perm)
}

func (s *LocalSession) UploadFile(rel, localPath string, perm os.FileMode) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(abs), 0755); err != nil {
		return err
	}
	src, err := os.Open(localPath)
	if err != nil {
		return err
	}
	defer src.Close()
	dst, err := os.OpenFile(abs, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, perm)
	if err != nil {
		return err
	}
	if _, err := io.Copy(dst, src); err != nil {
		_ = dst.Close()
		return err
	}
	return dst.Close()
}

func (s *LocalSession) UploadTree(relPrefix, localDir string) error {
	prefix, err := manifest.NormalizeRelativePath(relPrefix)
	if err != nil {
		return err
	}
	return filepath.WalkDir(localDir, func(path string, d os.DirEntry, walkErr error) error {
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
		remoteRel := filepath.ToSlash(filepath.Join(prefix, rel))
		if d.IsDir() {
			return s.MkdirAll(remoteRel, 0755)
		}
		return s.UploadFile(remoteRel, path, 0644)
	})
}

func (s *LocalSession) UploadTreeTar(relPrefix, localDir string) error {
	return s.UploadTree(relPrefix, localDir)
}

func (s *LocalSession) DownloadTree(relPrefix, localDir string) error {
	prefix, err := manifest.NormalizeRelativePath(relPrefix)
	if err != nil {
		return err
	}
	srcRoot, err := s.abs(prefix)
	if err != nil {
		return err
	}
	return copyTreeLocal(srcRoot, localDir)
}

func copyTreeLocal(srcRoot, dstRoot string) error {
	return filepath.WalkDir(srcRoot, func(path string, d os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		rel, err := filepath.Rel(srcRoot, path)
		if err != nil {
			return err
		}
		if rel == "." {
			return nil
		}
		target := filepath.Join(dstRoot, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0755)
		}
		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			return err
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		return os.WriteFile(target, data, 0644)
	})
}

func (s *LocalSession) ReadManifest() (*manifest.Document, error) {
	path := filepath.Join(s.Root, filepath.FromSlash(manifest.RelativePath))
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return manifest.Unmarshal(data)
}

func (s *LocalSession) WriteManifest(doc *manifest.Document) error {
	data, err := manifest.Marshal(doc)
	if err != nil {
		return err
	}
	dir := filepath.Join(s.Root, ".tpcectl")
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	final := filepath.Join(dir, "deploy-manifest.json")
	tmp := final + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, final)
}

func (s *LocalSession) Remove(rel string) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	return os.Remove(abs)
}

func (s *LocalSession) RemoveAll(rel string) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	return os.RemoveAll(abs)
}

func (s *LocalSession) RemoveEmptyDir(rel string) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	entries, err := os.ReadDir(abs)
	if err != nil {
		return err
	}
	if len(entries) > 0 {
		return fmt.Errorf("directory %s is not empty", rel)
	}
	return os.Remove(abs)
}

func (s *LocalSession) Exists(rel string) (bool, error) {
	abs, err := s.abs(rel)
	if err != nil {
		return false, err
	}
	_, err = os.Stat(abs)
	if err == nil {
		return true, nil
	}
	if os.IsNotExist(err) {
		return false, nil
	}
	return false, err
}

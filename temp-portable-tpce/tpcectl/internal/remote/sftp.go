package remote

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/pkg/sftp"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/manifest"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/sshx"
)

// Session performs file operations under remote_root on one host.
type Session interface {
	HostName() string
	RemoteRoot() string
	Close() error
	EnsureRoot() error
	MkdirAll(rel string, perm os.FileMode) error
	UploadFile(rel, localPath string, perm os.FileMode) error
	UploadTree(relPrefix, localDir string) error
	UploadTreeTar(relPrefix, localDir string) error
	DownloadTree(relPrefix, localDir string) error
	ReadManifest() (*manifest.Document, error)
	WriteManifest(doc *manifest.Document) error
	Remove(rel string) error
	RemoveAll(rel string) error
	RemoveEmptyDir(rel string) error
	Exists(rel string) (bool, error)
}

// Dial opens an SSH/SFTP session for hostName.
func Dial(profileHost string, cfg sshx.HostConfig, remoteRoot string) (Session, error) {
	ex, err := DialExecutor(profileHost, cfg, remoteRoot)
	if err != nil {
		return nil, err
	}
	return ex, nil
}

func newSFTPClient(client *sshx.Client) (*sftp.Client, error) {
	return sftp.NewClient(client.SSHConn())
}

type sftpSession struct {
	hostName   string
	remoteRoot string
	ssh        *sshx.Client
	sftp       *sftp.Client
}

func (s *sftpSession) HostName() string  { return s.hostName }
func (s *sftpSession) RemoteRoot() string { return s.remoteRoot }

func (s *sftpSession) Close() error {
	if s.sftp != nil {
		_ = s.sftp.Close()
	}
	return s.ssh.Close()
}

func (s *sftpSession) EnsureRoot() error {
	return s.sftp.MkdirAll(s.remoteRoot)
}

func (s *sftpSession) Run(cmd string) ([]byte, error) {
	return s.ssh.Run(cmd)
}

func (s *sftpSession) StartDetached(cmd string) error {
	return s.ssh.StartDetached(cmd)
}

func (s *sftpSession) WriteBytes(rel string, data []byte, perm os.FileMode) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	if err := s.sftp.MkdirAll(filepath.Dir(abs)); err != nil {
		return err
	}
	tmp := abs + ".tmp"
	dst, err := s.sftp.Create(tmp)
	if err != nil {
		return err
	}
	if _, err := dst.Write(data); err != nil {
		_ = dst.Close()
		return err
	}
	if err := dst.Close(); err != nil {
		return err
	}
	if err := s.sftp.Chmod(tmp, perm); err != nil {
		return err
	}
	return s.sftp.Rename(tmp, abs)
}

func (s *sftpSession) ReadBytes(rel string) ([]byte, error) {
	abs, err := s.abs(rel)
	if err != nil {
		return nil, err
	}
	f, err := s.sftp.Open(abs)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return io.ReadAll(f)
}

func (s *sftpSession) abs(rel string) (string, error) {
	norm, err := manifest.NormalizeRelativePath(rel)
	if err != nil {
		return "", err
	}
	return filepath.Join(s.remoteRoot, filepath.FromSlash(norm)), nil
}

func (s *sftpSession) MkdirAll(rel string, perm os.FileMode) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	return s.sftp.MkdirAll(abs)
}

func (s *sftpSession) UploadFile(rel, localPath string, perm os.FileMode) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	if err := s.sftp.MkdirAll(filepath.Dir(abs)); err != nil {
		return err
	}
	src, err := os.Open(localPath)
	if err != nil {
		return err
	}
	defer src.Close()

	dst, err := s.sftp.Create(abs)
	if err != nil {
		return err
	}
	if _, err := io.Copy(dst, src); err != nil {
		_ = dst.Close()
		return err
	}
	if err := dst.Close(); err != nil {
		return err
	}
	return s.sftp.Chmod(abs, perm)
}

func (s *sftpSession) UploadTree(relPrefix, localDir string) error {
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

func (s *sftpSession) UploadTreeTar(relPrefix, localDir string) error {
	prefix, err := manifest.NormalizeRelativePath(relPrefix)
	if err != nil {
		return err
	}
	remoteAbs, err := s.abs(prefix)
	if err != nil {
		return err
	}
	return s.ssh.UploadTar(localDir, remoteAbs)
}

func (s *sftpSession) manifestPath() string {
	return filepath.Join(s.remoteRoot, filepath.FromSlash(manifest.RelativePath))
}

func (s *sftpSession) ReadManifest() (*manifest.Document, error) {
	f, err := s.sftp.Open(s.manifestPath())
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(f)
	if err != nil {
		return nil, err
	}
	return manifest.Unmarshal(data)
}

func (s *sftpSession) WriteManifest(doc *manifest.Document) error {
	data, err := manifest.Marshal(doc)
	if err != nil {
		return err
	}
	dir := filepath.Join(s.remoteRoot, ".tpcectl")
	if err := s.sftp.MkdirAll(dir); err != nil {
		return err
	}
	final := s.manifestPath()
	tmp := final + ".tmp"
	if err := s.sftp.MkdirAll(filepath.Dir(final)); err != nil {
		return err
	}
	dst, err := s.sftp.Create(tmp)
	if err != nil {
		return err
	}
	if _, err := dst.Write(data); err != nil {
		_ = dst.Close()
		return err
	}
	if err := dst.Close(); err != nil {
		return err
	}
	if err := s.sftp.Chmod(tmp, 0600); err != nil {
		return err
	}
	return s.sftp.Rename(tmp, final)
}

func (s *sftpSession) Remove(rel string) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	return s.sftp.Remove(abs)
}

func (s *sftpSession) RemoveAll(rel string) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	_, err = s.ssh.Run(fmt.Sprintf("rm -rf %q", abs))
	return err
}

func (s *sftpSession) RemoveEmptyDir(rel string) error {
	abs, err := s.abs(rel)
	if err != nil {
		return err
	}
	entries, err := s.sftp.ReadDir(abs)
	if err != nil {
		return err
	}
	if len(entries) > 0 {
		return fmt.Errorf("directory %s is not empty", rel)
	}
	return s.sftp.RemoveDirectory(abs)
}

func (s *sftpSession) Exists(rel string) (bool, error) {
	abs, err := s.abs(rel)
	if err != nil {
		return false, err
	}
	_, err = s.sftp.Stat(abs)
	if err == nil {
		return true, nil
	}
	if isNotExist(err) {
		return false, nil
	}
	return false, err
}

func isNotExist(err error) bool {
	return strings.Contains(err.Error(), "file does not exist") ||
		strings.Contains(err.Error(), "no such file")
}

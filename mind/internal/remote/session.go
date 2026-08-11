package remote

import (
	"io"
	"os"
	"time"
)

// Session is a control-plane connection to one runtime host.
type Session interface {
	// Key is the profile host address (session identity; co-located
	// loaders/workers share the same key).
	Key() string
	// Address is the network address used to reach the host.
	Address() string
	// Upload copies a local file to a remote absolute-or-root-relative path.
	Upload(localPath, remotePath string) error
	// Download copies a remote file to a local path.
	Download(remotePath, localPath string) error
	// ReadFile reads a remote file.
	ReadFile(remotePath string) ([]byte, error)
	// WriteFile writes a remote file (creates parent directories).
	WriteFile(remotePath string, data []byte) error
	// WriteFileMode writes a remote file with explicit permission bits.
	WriteFileMode(remotePath string, data []byte, mode os.FileMode) error
	// MkdirAll creates a remote directory tree.
	MkdirAll(remotePath string) error
	// Exists reports whether a remote path exists.
	Exists(remotePath string) (bool, error)
	// Remove removes a remote file if present.
	Remove(remotePath string) error
	// RemoveAll removes a remote file or directory tree if present.
	RemoveAll(remotePath string) error
	// StartDetached starts a process without waiting; returns OS pid when known.
	StartDetached(workDir, binary string, argv []string, env map[string]string, stdoutPath, stderrPath string) (pid int, err error)
	// Signal sends a signal to a remote pid (e.g. "TERM", "KILL").
	Signal(pid int, sig string) error
	// IsAlive reports whether pid appears to be running.
	IsAlive(pid int) (bool, error)
	// Close releases the session.
	Close() error
}

// DialConfig configures how sessions are opened.
type DialConfig struct {
	User               string
	KnownHostsPath     string
	InsecureIgnoreHost bool
	ConnectTimeout     time.Duration
	// LocalRoot is the filesystem root used by Local sessions (expanded remote_root).
	LocalRoot string
	// UseAgent enables SSH agent auth when available.
	UseAgent bool
}

// CopyReader is a small helper for tests/upload.
func CopyReader(r io.Reader, w io.Writer) error {
	_, err := io.Copy(w, r)
	return err
}

package remote

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/sshx"
)

// Executor extends Session with remote command and byte I/O.
type Executor interface {
	Session
	Run(cmd string) ([]byte, error)
	StartDetached(cmd string) error
	WriteBytes(rel string, data []byte, perm os.FileMode) error
	ReadBytes(rel string) ([]byte, error)
}

// ExecutorDialer opens an Executor for a logical host.
type ExecutorDialer func(hostName string, profile *config.ResolvedProfile) (Executor, error)

// DefaultExecutorDialer uses SSH/SFTP.
func DefaultExecutorDialer() ExecutorDialer {
	return func(hostName string, profile *config.ResolvedProfile) (Executor, error) {
		cfg, err := sshx.ResolveHostConfig(profile, hostName)
		if err != nil {
			return nil, err
		}
		return DialExecutor(hostName, cfg, profile.Paths.RemoteRoot)
	}
}

// DialExecutor opens SSH/SFTP executor session.
func DialExecutor(profileHost string, cfg sshx.HostConfig, remoteRoot string) (Executor, error) {
	client, err := sshx.Dial(profileHost, cfg)
	if err != nil {
		return nil, err
	}
	sftpClient, err := newSFTPClient(client)
	if err != nil {
		_ = client.Close()
		return nil, err
	}
	return &sftpSession{
		hostName:   profileHost,
		remoteRoot: remoteRoot,
		ssh:        client,
		sftp:       sftpClient,
	}, nil
}

// WriteBytes uploads in-memory content to a relative path.
func WriteBytes(session Session, rel string, data []byte, perm os.FileMode) error {
	if w, ok := session.(interface {
		WriteBytes(string, []byte, os.FileMode) error
	}); ok {
		return w.WriteBytes(rel, data, perm)
	}
	return fmt.Errorf("session does not support WriteBytes")
}

// ReadBytes reads a relative file from the session.
func ReadBytes(session Session, rel string) ([]byte, error) {
	if r, ok := session.(interface {
		ReadBytes(string) ([]byte, error)
	}); ok {
		return r.ReadBytes(rel)
	}
	return nil, fmt.Errorf("session does not support ReadBytes")
}

// FileSHA256 computes SHA-256 of a remote relative file.
func FileSHA256(session Session, rel string) (string, error) {
	data, err := ReadBytes(session, rel)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:]), nil
}

// JoinRemoteAbs returns an absolute remote path for display/shell use.
func JoinRemoteAbs(remoteRoot, rel string) string {
	rel = filepath.ToSlash(rel)
	if filepath.IsAbs(rel) {
		return rel
	}
	return filepath.ToSlash(filepath.Join(remoteRoot, rel))
}

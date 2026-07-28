package state

import (
	"fmt"
	"os"
	"path/filepath"
	"syscall"
)

// ProfileLock serializes runs for one profile (spec-orchestrator §14.2.4).
type ProfileLock struct {
	path *os.File
}

// AcquireProfileLock creates an exclusive lock file for profileID.
func (s *Store) AcquireProfileLock(profileID string) (*ProfileLock, error) {
	dir := filepath.Join(s.Root, "profiles", profileID)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return nil, err
	}
	path := filepath.Join(dir, "run.lock")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		_ = f.Close()
		return nil, fmt.Errorf("profile %s already has an active run (lock held)", profileID)
	}
	return &ProfileLock{path: f}, nil
}

// Release unlocks and closes the lock file.
func (l *ProfileLock) Release() error {
	if l == nil || l.path == nil {
		return nil
	}
	err := syscall.Flock(int(l.path.Fd()), syscall.LOCK_UN)
	closeErr := l.path.Close()
	if err != nil {
		return err
	}
	return closeErr
}

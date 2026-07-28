package sshx

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// UploadTar streams a local directory to remoteDest via tar over SSH stdin.
func (c *Client) UploadTar(localDir, remoteDest string) error {
	if c == nil || c.conn == nil {
		return fmt.Errorf("ssh client is not connected")
	}
	localDir = filepath.Clean(localDir)
	session, err := c.conn.NewSession()
	if err != nil {
		return err
	}
	defer session.Close()

	stdin, err := session.StdinPipe()
	if err != nil {
		return err
	}

	errCh := make(chan error, 1)
	go func() {
		cmd := exec.Command("tar", "czf", "-", "-C", localDir, ".")
		cmd.Stdout = stdin
		cmd.Stderr = os.Stderr
		errCh <- cmd.Run()
		_ = stdin.Close()
	}()

	remoteCmd := fmt.Sprintf("mkdir -p %q && tar xzf - -C %q", remoteDest, remoteDest)
	if err := session.Start(remoteCmd); err != nil {
		return err
	}
	if err := <-errCh; err != nil {
		_ = session.Close()
		return fmt.Errorf("local tar: %w", err)
	}
	return session.Wait()
}

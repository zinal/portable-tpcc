package wait

import (
	"context"
	"fmt"
	"net"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/remote"
)

// TCPListen polls until host:port accepts a TCP connection or timeout expires.
func TCPListen(ctx context.Context, host string, port int) error {
	addr := fmt.Sprintf("%s:%d", host, port)
	deadline, ok := ctx.Deadline()
	if !ok {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, 60*time.Second)
		defer cancel()
		deadline, _ = ctx.Deadline()
	}
	for {
		if err := ctx.Err(); err != nil {
			return fmt.Errorf("timed out waiting for listen on %s: %w", addr, err)
		}
		conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
		if err == nil {
			_ = conn.Close()
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("timed out waiting for listen on %s", addr)
		}
		time.Sleep(500 * time.Millisecond)
	}
}

// RemoteFile polls until a remote relative path exists.
func RemoteFile(ctx context.Context, ex remote.Executor, rel string) error {
	for {
		if err := ctx.Err(); err != nil {
			return fmt.Errorf("timed out waiting for %s on %s: %w", rel, ex.HostName(), err)
		}
		ok, err := ex.Exists(rel)
		if err != nil {
			return err
		}
		if ok {
			return nil
		}
		time.Sleep(500 * time.Millisecond)
	}
}

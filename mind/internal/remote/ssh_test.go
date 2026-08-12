package remote

import (
	"io"
	"strings"
	"testing"
	"time"
)

func TestSSHStartDetachedRejectsInvalidEnvKey(t *testing.T) {
	s := &SSH{}
	_, err := s.StartDetached("/tmp", "/bin/true", nil, map[string]string{
		"BAD;touch_x": "secret",
	}, "/tmp/stdout", "/tmp/stderr")
	if err == nil || !strings.Contains(err.Error(), "invalid environment variable name") {
		t.Fatalf("expected invalid env name error, got %v", err)
	}
}

func TestStartDetachedShellCmd(t *testing.T) {
	cmd, err := startDetachedShellCmd(
		"remote/run-1",
		"remote/tpcc-oceanbase",
		[]string{"loader", "--instance", "host-a-l"},
		map[string]string{"DB_PASSWORD": "s3cret"},
		"remote/run-1/loader/host-a-l/stdout.log",
		"remote/run-1/loader/host-a-l/stderr.log",
	)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		"cd 'remote/run-1' && ",
		"DB_PASSWORD='s3cret' ",
		"nohup '../tpcc-oceanbase'",
		" 'loader' '--instance' 'host-a-l'",
		" > 'loader/host-a-l/stdout.log'",
		" 2> 'loader/host-a-l/stderr.log'",
		" < /dev/null & echo $!",
	} {
		if !strings.Contains(cmd, want) {
			t.Fatalf("command %q missing %q", cmd, want)
		}
	}
}

func TestStartDetachedShellCmdRejectsInvalidEnvKey(t *testing.T) {
	_, err := startDetachedShellCmd("/tmp", "/bin/true", nil, map[string]string{
		"BAD;touch_x": "secret",
	}, "/tmp/stdout", "/tmp/stderr")
	if err == nil || !strings.Contains(err.Error(), "invalid environment variable name") {
		t.Fatalf("expected invalid env name error, got %v", err)
	}
}

func TestReadRemotePID(t *testing.T) {
	pid, err := readRemotePID(strings.NewReader("4321\n"), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if pid != 4321 {
		t.Fatalf("pid=%d, want 4321", pid)
	}
}

func TestReadRemotePIDTimeout(t *testing.T) {
	r, w := io.Pipe()
	defer r.Close()
	defer w.Close()
	_, err := readRemotePID(r, 20*time.Millisecond)
	if err == nil || !strings.Contains(err.Error(), "timeout") {
		t.Fatalf("expected timeout, got %v", err)
	}
}

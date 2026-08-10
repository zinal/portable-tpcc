package remote

import (
	"strings"
	"testing"
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

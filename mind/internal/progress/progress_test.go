package progress

import (
	"bytes"
	"strings"
	"testing"
	"time"
)

func TestPrintfWritesTimestampedLine(t *testing.T) {
	var buf bytes.Buffer
	SetWriter(&buf)
	defer SetWriter(nil)
	clock = func() time.Time { return time.Date(2026, 8, 11, 12, 0, 0, 0, time.UTC) }
	defer func() { clock = func() time.Time { return time.Now().UTC() } }()

	Printf("deploying to %d hosts", 2)
	got := buf.String()
	if !strings.HasPrefix(got, "2026-08-11T12:00:00Z deploying to 2 hosts\n") {
		t.Fatalf("unexpected progress line: %q", got)
	}
}

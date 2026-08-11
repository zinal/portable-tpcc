// Package progress writes human-readable operation progress for mind-tpcc.
// Output goes to stderr by default so JSON stdout commands stay machine-readable.
package progress

import (
	"fmt"
	"io"
	"os"
	"sync"
	"time"
)

// Writer is the destination for progress lines. Tests may replace it.
var (
	mu     sync.Mutex
	writer io.Writer = os.Stderr
	clock  = func() time.Time { return time.Now().UTC() }
)

// SetWriter replaces the progress destination. Pass nil to restore os.Stderr.
func SetWriter(w io.Writer) {
	mu.Lock()
	defer mu.Unlock()
	if w == nil {
		writer = os.Stderr
		return
	}
	writer = w
}

// Printf writes a timestamped progress line.
func Printf(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	mu.Lock()
	w := writer
	ts := clock().Format(time.RFC3339)
	mu.Unlock()
	fmt.Fprintf(w, "%s %s\n", ts, msg)
}

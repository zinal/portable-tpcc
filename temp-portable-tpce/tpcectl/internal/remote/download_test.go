package remote

import (
	"os"
	"path/filepath"
	"testing"
)

func TestDownloadTreeLocal(t *testing.T) {
	root := t.TempDir()
	remoteRoot := filepath.Join(root, "remote")
	if err := os.MkdirAll(filepath.Join(remoteRoot, "runs/x/bh1"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(remoteRoot, "runs/x/bh1/log.txt"), []byte("data"), 0644); err != nil {
		t.Fatal(err)
	}
	sess := &LocalSession{Host: "h1", Root: remoteRoot}
	dest := filepath.Join(root, "out")
	if err := sess.DownloadTree("runs/x/bh1", dest); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(dest, "log.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "data" {
		t.Fatalf("got %q", got)
	}
}

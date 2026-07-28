package manifest_test

import (
	"testing"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/manifest"
)

func TestValidateRelativePathRejectsTraversal(t *testing.T) {
	cases := []string{"../etc/passwd", "/abs/path", "..", "bin/../../etc"}
	for _, c := range cases {
		if err := manifest.ValidateRelativePath(c); err == nil {
			t.Fatalf("expected error for %q", c)
		}
	}
}

func TestNormalizeRelativePath(t *testing.T) {
	got, err := manifest.NormalizeRelativePath("bin/Driver.exe")
	if err != nil || got != "bin/Driver.exe" {
		t.Fatalf("normalize: %q err=%v", got, err)
	}
}

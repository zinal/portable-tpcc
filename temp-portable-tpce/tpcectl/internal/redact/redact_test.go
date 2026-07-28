package redact

import "testing"

func TestStringConninfoPassword(t *testing.T) {
	in := `Loader.exe -p "host=db port=5432 password='s3cr\\'et' user=u"`
	got := String(in)
	if stringsContains(got, "s3cr") || stringsContains(got, "password='") {
		t.Fatalf("password not redacted: %q", got)
	}
	if !stringsContains(got, "password=REDACTED") {
		t.Fatalf("missing redaction marker: %q", got)
	}
}

func TestProfileYAML(t *testing.T) {
	in := []byte("db:\n  password: not-for-collect\n  password_env: TPCE_PGPASSWORD\n")
	got := ProfileYAML(in)
	if stringsContains(string(got), "not-for-collect") {
		t.Fatalf("inline password not redacted: %s", got)
	}
	if !stringsContains(string(got), "password_env: TPCE_PGPASSWORD") {
		t.Fatalf("password_env name should remain: %s", got)
	}
}

func stringsContains(s, sub string) bool {
	return len(sub) == 0 || (len(s) >= len(sub) && indexOf(s, sub) >= 0)
}

func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

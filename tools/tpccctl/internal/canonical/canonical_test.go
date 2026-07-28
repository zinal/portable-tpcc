package canonical_test

import (
	"testing"

	"portable-tpcc/tools/tpccctl/internal/canonical"
)

func TestCanonicalSHA256_deterministic(t *testing.T) {
	v := map[string]interface{}{
		"b": 2,
		"a": 1,
		"c": []interface{}{3, 1},
	}
	h1, err := canonical.SHA256(v)
	if err != nil {
		t.Fatal(err)
	}
	h2, err := canonical.SHA256(v)
	if err != nil {
		t.Fatal(err)
	}
	if h1 != h2 {
		t.Fatalf("hashes differ: %s vs %s", h1, h2)
	}
	if len(h1) != 64 {
		t.Fatalf("hash length %d, want 64", len(h1))
	}
}

func TestCanonical_keyOrder(t *testing.T) {
	a := map[string]interface{}{"a": 1, "b": 2}
	b := map[string]interface{}{"b": 2, "a": 1}
	ha, err := canonical.SHA256(a)
	if err != nil {
		t.Fatal(err)
	}
	hb, err := canonical.SHA256(b)
	if err != nil {
		t.Fatal(err)
	}
	if ha != hb {
		t.Fatalf("key order affected hash: %s vs %s", ha, hb)
	}
}

func TestMarshalJSON_integers(t *testing.T) {
	data, err := canonical.MarshalJSON(map[string]interface{}{"n": float64(42)})
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != `{"n":42}` {
		t.Fatalf("got %s", string(data))
	}
}

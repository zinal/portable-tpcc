package schema

import (
	"testing"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

func TestPreprocessPartitionedSQL(t *testing.T) {
	in := `\if :{?partitions}
\else
\set partitions 32
\endif
SELECT pg_temp.tpce_hash_parts('trade', :partitions);
`
	got := preprocessPartitionedSQL(in, 16)
	if stringsContains(got, `\set`) || stringsContains(got, `:partitions`) {
		t.Fatalf("psql meta or variable left in output:\n%s", got)
	}
	if !stringsContains(got, "tpce_hash_parts('trade', 16)") {
		t.Fatalf("partition substitution missing:\n%s", got)
	}
}

func TestIndexesDeferred(t *testing.T) {
	noShards := &config.ResolvedProfile{Profile: config.Profile{}}
	if IndexesDeferred(noShards) {
		t.Fatal("expected no deferral without load shards")
	}
	withShards := &config.ResolvedProfile{
		Profile: config.Profile{
			Load: config.LoadConfig{
				Shards: []config.LoadShard{{Host: "load1", Begin: 1, Count: 1000}},
			},
		},
	}
	if !IndexesDeferred(withShards) {
		t.Fatal("expected deferral when load shards exist")
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

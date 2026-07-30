package histogram_test

import (
	"testing"

	"portable-tpcc/tools/tpccctl/internal/histogram"
)

func TestMergeAndPercentile(t *testing.T) {
	a := histogram.Raw{
		Layout: "linear_exp", Unit: "ms", HdrTill: 4, MaxValue: 64,
		TotalCount: 4, Buckets: []uint64{1, 1, 1, 1, 0, 0, 0},
	}
	b := histogram.Raw{
		Layout: "linear_exp", Unit: "ms", HdrTill: 4, MaxValue: 64,
		TotalCount: 4, Buckets: []uint64{0, 0, 0, 0, 4, 0, 0},
	}
	var m histogram.Raw
	if err := histogram.Merge(&m, a); err != nil {
		t.Fatal(err)
	}
	if err := histogram.Merge(&m, b); err != nil {
		t.Fatal(err)
	}
	if m.TotalCount != 8 {
		t.Fatalf("total %d", m.TotalCount)
	}
	p50, err := histogram.ValueAtPercentile(m, 50)
	if err != nil {
		t.Fatal(err)
	}
	if p50 == 0 {
		t.Fatal("expected non-zero p50")
	}
	pct, err := histogram.Percentiles(m)
	if err != nil {
		t.Fatal(err)
	}
	if pct["p99"] == 0 {
		t.Fatal("expected non-zero p99")
	}
}

func TestMergeMismatch(t *testing.T) {
	var m histogram.Raw
	_ = histogram.Merge(&m, histogram.Raw{HdrTill: 2, MaxValue: 8, Buckets: []uint64{1, 0, 0, 0}})
	err := histogram.Merge(&m, histogram.Raw{HdrTill: 4, MaxValue: 8, Buckets: []uint64{1}})
	if err == nil {
		t.Fatal("expected mismatch")
	}
}

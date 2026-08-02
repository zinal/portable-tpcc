package histogram_test

import (
	"strings"
	"testing"

	"portable-tpcc/tools/tpccctl/internal/histogram"
)

func validHist(total uint64, buckets []uint64) histogram.Raw {
	return histogram.Raw{
		Layout: "linear_exp", Unit: "ms", HdrTill: 4, MaxValue: 64,
		TotalCount: total, Buckets: buckets,
	}
}

func TestExpectedBucketCount(t *testing.T) {
	n, err := histogram.ExpectedBucketCount(4, 64)
	if err != nil {
		t.Fatal(err)
	}
	if n != 9 {
		t.Fatalf("expected 9 buckets, got %d", n)
	}
	n, err = histogram.ExpectedBucketCount(4096, 32768)
	if err != nil {
		t.Fatal(err)
	}
	if n != 4100 {
		t.Fatalf("expected 4100 buckets, got %d", n)
	}
	if _, err := histogram.ExpectedBucketCount(0, 64); err == nil {
		t.Fatal("expected error for hdr_till=0")
	}
}

func TestValidateRejectsInconsistentPayload(t *testing.T) {
	h := validHist(4, []uint64{1, 1, 1, 1, 0, 0, 0, 0, 0})
	if err := histogram.Validate(h); err != nil {
		t.Fatal(err)
	}

	bad := h
	bad.Layout = ""
	if err := histogram.Validate(bad); err == nil || !strings.Contains(err.Error(), "layout") {
		t.Fatalf("expected missing layout error, got %v", err)
	}

	bad = h
	bad.Unit = ""
	if err := histogram.Validate(bad); err == nil || !strings.Contains(err.Error(), "unit") {
		t.Fatalf("expected missing unit error, got %v", err)
	}

	bad = h
	bad.Buckets = []uint64{1, 1, 1, 1}
	if err := histogram.Validate(bad); err == nil || !strings.Contains(err.Error(), "bucket length") {
		t.Fatalf("expected bucket length error, got %v", err)
	}

	bad = h
	bad.TotalCount = 99
	if err := histogram.Validate(bad); err == nil || !strings.Contains(err.Error(), "total_count") {
		t.Fatalf("expected total_count error, got %v", err)
	}
}

func TestMergeAndPercentile(t *testing.T) {
	a := validHist(4, []uint64{1, 1, 1, 1, 0, 0, 0, 0, 0})
	b := validHist(4, []uint64{0, 0, 0, 0, 4, 0, 0, 0, 0})
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
	_ = histogram.Merge(&m, validHist(1, []uint64{1, 0, 0, 0, 0, 0, 0, 0, 0}))
	err := histogram.Merge(&m, histogram.Raw{
		Layout: "linear_exp", Unit: "ms", HdrTill: 2, MaxValue: 8,
		TotalCount: 1, Buckets: []uint64{1, 0, 0, 0, 0},
	})
	if err == nil {
		t.Fatal("expected mismatch")
	}
}

func TestMergeRejectsBucketLengthMismatch(t *testing.T) {
	var m histogram.Raw
	if err := histogram.Merge(&m, validHist(1, []uint64{1, 0, 0, 0, 0, 0, 0, 0, 0})); err != nil {
		t.Fatal(err)
	}
	short := validHist(1, []uint64{1, 0, 0, 0, 0, 0, 0, 0, 0})
	short.Buckets = append([]uint64{}, short.Buckets[:8]...)
	short.TotalCount = 1
	// Force equal total but wrong length after bypassing construction helper.
	err := histogram.Merge(&m, short)
	if err == nil {
		t.Fatal("expected bucket length rejection")
	}
}

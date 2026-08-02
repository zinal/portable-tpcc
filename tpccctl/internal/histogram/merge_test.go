package histogram_test

import (
	"strings"
	"testing"

	"portable-tpcc/tpccctl/internal/histogram"
)

func validHist(total uint64, buckets []uint64) histogram.Raw {
	return histogram.Raw{
		Layout: "linear_exp", Unit: "ms", HdrTill: 4, MaxValue: 64,
		TotalCount: total, MinRecorded: 0, MaxRecorded: 3, SumValues: 6,
		Buckets: buckets,
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

	bad = h
	bad.SumValues = 0
	if err := histogram.Validate(bad); err == nil || !strings.Contains(err.Error(), "sum_values") {
		t.Fatalf("expected sum_values error, got %v", err)
	}

	bad = h
	bad.MinRecorded = 10
	bad.MaxRecorded = 3
	bad.SumValues = 20
	if err := histogram.Validate(bad); err == nil || !strings.Contains(err.Error(), "min_recorded") {
		t.Fatalf("expected min_recorded error, got %v", err)
	}
}

func TestMergeAndPercentile(t *testing.T) {
	a := validHist(4, []uint64{1, 1, 1, 1, 0, 0, 0, 0, 0})
	b := histogram.Raw{
		Layout: "linear_exp", Unit: "ms", HdrTill: 4, MaxValue: 64,
		TotalCount: 4, MinRecorded: 4, MaxRecorded: 7, SumValues: 22,
		Buckets: []uint64{0, 0, 0, 0, 4, 0, 0, 0, 0},
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
	if m.MinRecorded != 0 {
		t.Fatalf("min %d", m.MinRecorded)
	}
	if m.MaxRecorded != 7 {
		t.Fatalf("max %d", m.MaxRecorded)
	}
	if m.SumValues != 28 {
		t.Fatalf("sum %d", m.SumValues)
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
	stats, err := histogram.ReportStats(m)
	if err != nil {
		t.Fatal(err)
	}
	if stats["min"].(uint64) != 0 || stats["max"].(uint64) != 7 {
		t.Fatalf("unexpected min/max in report: %#v", stats)
	}
	if avg := stats["avg"].(float64); avg != 3.5 {
		t.Fatalf("expected avg 3.5, got %v", avg)
	}
}

func TestMergeMismatch(t *testing.T) {
	var m histogram.Raw
	_ = histogram.Merge(&m, validHist(1, []uint64{1, 0, 0, 0, 0, 0, 0, 0, 0}))
	err := histogram.Merge(&m, histogram.Raw{
		Layout: "linear_exp", Unit: "ms", HdrTill: 2, MaxValue: 8,
		TotalCount: 1, MinRecorded: 0, MaxRecorded: 0, SumValues: 0,
		Buckets: []uint64{1, 0, 0, 0, 0},
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

func TestMergeEmptyDoesNotClobberMin(t *testing.T) {
	a := histogram.Raw{
		Layout: "linear_exp", Unit: "ms", HdrTill: 4, MaxValue: 64,
		TotalCount: 2, MinRecorded: 5, MaxRecorded: 8, SumValues: 13,
		Buckets: []uint64{0, 0, 0, 0, 1, 1, 0, 0, 0},
	}
	empty := histogram.Raw{
		Layout: "linear_exp", Unit: "ms", HdrTill: 4, MaxValue: 64,
		TotalCount: 0, MinRecorded: 0, MaxRecorded: 0, SumValues: 0,
		Buckets: []uint64{0, 0, 0, 0, 0, 0, 0, 0, 0},
	}
	var m histogram.Raw
	if err := histogram.Merge(&m, a); err != nil {
		t.Fatal(err)
	}
	if err := histogram.Merge(&m, empty); err != nil {
		t.Fatal(err)
	}
	if m.MinRecorded != 5 || m.MaxRecorded != 8 || m.SumValues != 13 {
		t.Fatalf("empty merge clobbered extrema/sum: %#v", m)
	}
}

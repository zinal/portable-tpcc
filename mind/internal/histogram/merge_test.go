package histogram_test

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	"portable-tpcc/mind/internal/histogram"
)

func bucketCount(hdrTill, maxValue uint64) int {
	n, err := histogram.ExpectedBucketCount(hdrTill, maxValue)
	if err != nil {
		panic(err)
	}
	return n
}

func emptyBuckets(hdrTill, maxValue uint64) []uint64 {
	return make([]uint64, bucketCount(hdrTill, maxValue))
}

func validHist(total uint64, prefix []uint64) histogram.Raw {
	h := histogram.Raw{
		Layout:              "linear_exp",
		Unit:                "ms",
		HdrTill:             4,
		MaxValue:            64,
		SubBucketsPerOctave: histogram.SubBucketsPerOctave,
		TotalCount:          total,
		OverflowCount:       0,
		Buckets:             emptyBuckets(4, 64),
	}
	copy(h.Buckets, prefix)
	if total == 0 {
		return h
	}
	first, last := -1, -1
	for i, c := range h.Buckets {
		if c == 0 {
			continue
		}
		if first < 0 {
			first = i
		}
		last = i
	}
	if first < 0 {
		return h
	}
	h.MinRecorded = uint64(first)
	h.MaxRecorded = uint64(last)
	var sum uint64
	for i := first; i <= last; i++ {
		sum += uint64(i) * h.Buckets[i]
	}
	h.SumValues = sum
	return h
}

func TestExpectedBucketCount(t *testing.T) {
	n, err := histogram.ExpectedBucketCount(4, 64)
	if err != nil {
		t.Fatal(err)
	}
	if n != 64 {
		t.Fatalf("expected 64 buckets, got %d", n)
	}
	n, err = histogram.ExpectedBucketCount(4096, 32768)
	if err != nil {
		t.Fatal(err)
	}
	if n != 4288 {
		t.Fatalf("expected 4288 buckets, got %d", n)
	}
	n, err = histogram.ExpectedBucketCount(4096, 120000000)
	if err != nil {
		t.Fatal(err)
	}
	if n != 5056 {
		t.Fatalf("expected 5056 buckets, got %d", n)
	}
	if _, err := histogram.ExpectedBucketCount(0, 64); err == nil {
		t.Fatal("expected error for hdr_till=0")
	}
}

func TestValidateRejectsInconsistentPayload(t *testing.T) {
	h := validHist(4, []uint64{1, 1, 1, 1})
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

	empty := validHist(0, nil)
	empty.MaxRecorded = 99
	if err := histogram.Validate(empty); err == nil || !strings.Contains(err.Error(), "empty histogram") {
		t.Fatalf("expected empty extrema error, got %v", err)
	}

	zeroed := h
	zeroed.MinRecorded = 0
	zeroed.MaxRecorded = 0
	zeroed.SumValues = 0
	if err := histogram.Validate(zeroed); err == nil || !strings.Contains(err.Error(), "max_recorded") {
		t.Fatalf("expected zero extrema vs occupied buckets error, got %v", err)
	}
}

func TestMergeAndPercentile(t *testing.T) {
	a := validHist(4, []uint64{1, 1, 1, 1})
	bPrefix := make([]uint64, 8)
	bPrefix[4], bPrefix[5], bPrefix[6], bPrefix[7] = 1, 1, 1, 1
	b := validHist(4, bPrefix)
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
	if stats["overflow_count"].(uint64) != 0 {
		t.Fatalf("expected zero overflow, got %#v", stats["overflow_count"])
	}
}

func TestMergeMismatch(t *testing.T) {
	var m histogram.Raw
	_ = histogram.Merge(&m, validHist(1, []uint64{1}))
	short := histogram.Raw{
		Layout:              "linear_exp",
		Unit:                "ms",
		HdrTill:             2,
		MaxValue:            8,
		SubBucketsPerOctave: histogram.SubBucketsPerOctave,
		TotalCount:          1,
		OverflowCount:       0,
		MinRecorded:         0,
		MaxRecorded:         0,
		SumValues:           0,
		Buckets:             emptyBuckets(2, 8),
	}
	short.Buckets[0] = 1
	err := histogram.Merge(&m, short)
	if err == nil {
		t.Fatal("expected mismatch")
	}
}

func TestMergeRejectsBucketLengthMismatch(t *testing.T) {
	var m histogram.Raw
	if err := histogram.Merge(&m, validHist(1, []uint64{1})); err != nil {
		t.Fatal(err)
	}
	short := validHist(1, []uint64{1})
	short.Buckets = append([]uint64{}, short.Buckets[:8]...)
	err := histogram.Merge(&m, short)
	if err == nil {
		t.Fatal("expected bucket length rejection")
	}
}

func TestMergeEmptyDoesNotClobberMin(t *testing.T) {
	prefix := make([]uint64, 8)
	prefix[4], prefix[5] = 1, 1
	a := validHist(2, prefix)
	a.MinRecorded = 4
	a.MaxRecorded = 5
	a.SumValues = 9
	empty := validHist(0, nil)
	var m histogram.Raw
	if err := histogram.Merge(&m, a); err != nil {
		t.Fatal(err)
	}
	if err := histogram.Merge(&m, empty); err != nil {
		t.Fatal(err)
	}
	if m.MinRecorded != 4 || m.MaxRecorded != 5 || m.SumValues != 9 {
		t.Fatalf("empty merge clobbered extrema/sum: %#v", m)
	}
}

func TestMergeRejectsCountOverflow(t *testing.T) {
	full := histogram.Raw{
		Layout:              "linear_exp",
		Unit:                "ms",
		HdrTill:             4,
		MaxValue:            64,
		SubBucketsPerOctave: histogram.SubBucketsPerOctave,
		TotalCount:          math.MaxUint64,
		OverflowCount:       0,
		MinRecorded:         0,
		MaxRecorded:         0,
		SumValues:           0,
		Buckets:             emptyBuckets(4, 64),
	}
	full.Buckets[0] = math.MaxUint64
	one := validHist(1, []uint64{1})
	var m histogram.Raw
	if err := histogram.Merge(&m, full); err != nil {
		t.Fatal(err)
	}
	if err := histogram.Merge(&m, one); err == nil || !strings.Contains(err.Error(), "overflow") {
		t.Fatalf("expected overflow, got %v", err)
	}
}

func TestUnmarshalJSONRequiresExtremaFields(t *testing.T) {
	missing := []byte(`{
		"layout":"linear_exp","unit":"ms","hdr_till":4,"max_value":64,
		"total_count":4,"buckets":[1,1,1,1]
	}`)
	var h histogram.Raw
	err := json.Unmarshal(missing, &h)
	if err == nil || !strings.Contains(err.Error(), "min_recorded") {
		t.Fatalf("expected missing min_recorded, got %v", err)
	}

	present := []byte(`{
		"layout":"linear_exp","unit":"ms","hdr_till":4,"max_value":64,
		"sub_buckets_per_octave":64,"total_count":4,"overflow_count":0,
		"min_recorded":0,"max_recorded":3,"sum_values":6,
		"buckets":` + bucketsJSON([]uint64{1, 1, 1, 1}) + `
	}`)
	if err := json.Unmarshal(present, &h); err != nil {
		t.Fatal(err)
	}
	if err := histogram.Validate(h); err != nil {
		t.Fatal(err)
	}
}

func TestUnmarshalJSONRequiresOverflowCount(t *testing.T) {
	payload := []byte(`{
		"layout":"linear_exp","unit":"ms","hdr_till":4,"max_value":64,
		"total_count":4,"min_recorded":0,"max_recorded":3,"sum_values":6,
		"buckets":` + bucketsJSON([]uint64{1, 1, 1, 1}) + `
	}`)
	var h histogram.Raw
	err := json.Unmarshal(payload, &h)
	if err == nil || !strings.Contains(err.Error(), "overflow_count") {
		t.Fatalf("expected missing overflow_count, got %v", err)
	}
}

func TestMergeComponents(t *testing.T) {
	a := validHist(2, []uint64{2})
	a.Components = map[string]histogram.Raw{
		"pure": validHist(2, []uint64{0, 2}),
	}
	b := validHist(2, []uint64{0, 0, 2})
	b.MinRecorded = 2
	b.MaxRecorded = 2
	b.SumValues = 4
	b.Components = map[string]histogram.Raw{
		"pure": validHist(2, []uint64{0, 0, 2}),
	}
	var m histogram.Raw
	if err := histogram.Merge(&m, a); err != nil {
		t.Fatal(err)
	}
	if err := histogram.Merge(&m, b); err != nil {
		t.Fatal(err)
	}
	if m.TotalCount != 4 {
		t.Fatalf("total %d", m.TotalCount)
	}
	pure, ok := m.Components["pure"]
	if !ok {
		t.Fatal("missing merged pure component")
	}
	if pure.TotalCount != 4 {
		t.Fatalf("pure total %d", pure.TotalCount)
	}
	stats, err := histogram.ReportStats(m)
	if err != nil {
		t.Fatal(err)
	}
	comps, ok := stats["components"].(map[string]interface{})
	if !ok {
		t.Fatalf("missing component stats: %#v", stats)
	}
	if _, ok := comps["pure"].(map[string]interface{}); !ok {
		t.Fatalf("missing pure stats: %#v", comps)
	}
}

func TestOverflowPercentileUsesMaxValueNotRecordedMax(t *testing.T) {
	h := validHist(90, []uint64{0, 90})
	h.OverflowCount = 10
	h.TotalCount = 100
	h.MaxRecorded = 100000
	h.MinRecorded = 1
	h.SumValues = 90 + 10*100000
	if err := histogram.Validate(h); err != nil {
		t.Fatal(err)
	}
	p99, err := histogram.ValueAtPercentile(h, 99)
	if err != nil {
		t.Fatal(err)
	}
	if p99 != 64 {
		t.Fatalf("p99=%d, want max_value 64", p99)
	}
	if p99 == h.MaxRecorded {
		t.Fatal("p99 must not be replaced with max_recorded")
	}
	stats, err := histogram.ReportStats(h)
	if err != nil {
		t.Fatal(err)
	}
	if stats["overflow_count"].(uint64) != 10 {
		t.Fatalf("overflow_count=%v", stats["overflow_count"])
	}
}

func TestRelativeErrorPastLinearRegion(t *testing.T) {
	h := histogram.Raw{
		Layout:              "linear_exp",
		Unit:                "us",
		HdrTill:             4096,
		MaxValue:            120000000,
		SubBucketsPerOctave: histogram.SubBucketsPerOctave,
		TotalCount:          1,
		OverflowCount:       0,
		MinRecorded:         283000,
		MaxRecorded:         283000,
		SumValues:           283000,
		Buckets:             emptyBuckets(4096, 120000000),
	}
	idx := histogram.BucketForValue(h, 283000)
	if idx < 0 {
		t.Fatal("sample should be in-range")
	}
	h.Buckets[idx] = 1
	if err := histogram.Validate(h); err != nil {
		t.Fatal(err)
	}
	p99, err := histogram.ValueAtPercentile(h, 99)
	if err != nil {
		t.Fatal(err)
	}
	if p99 < 283000 {
		t.Fatalf("p99=%d below sample", p99)
	}
	rel := float64(p99-283000) / 283000.0
	if rel > 0.02 {
		t.Fatalf("relative error %.4f for p99=%d", rel, p99)
	}
}

func bucketsJSON(prefix []uint64) string {
	b := emptyBuckets(4, 64)
	copy(b, prefix)
	raw, err := json.Marshal(b)
	if err != nil {
		panic(err)
	}
	return string(raw)
}

package histogram

import (
	"encoding/json"
	"fmt"
	"math"
)

const LayoutLinearExp = "linear_exp"

// Raw is the portable linear_exp histogram payload from worker result.json.
type Raw struct {
	Layout      string   `json:"layout"`
	Unit        string   `json:"unit"`
	HdrTill     uint64   `json:"hdr_till"`
	MaxValue    uint64   `json:"max_value"`
	TotalCount  uint64   `json:"total_count"`
	MinRecorded uint64   `json:"min_recorded"`
	MaxRecorded uint64   `json:"max_recorded"`
	SumValues   uint64   `json:"sum_values"`
	Buckets     []uint64 `json:"buckets"`
}

type rawJSON struct {
	Layout      string   `json:"layout"`
	Unit        string   `json:"unit"`
	HdrTill     uint64   `json:"hdr_till"`
	MaxValue    uint64   `json:"max_value"`
	TotalCount  uint64   `json:"total_count"`
	MinRecorded *uint64  `json:"min_recorded"`
	MaxRecorded *uint64  `json:"max_recorded"`
	SumValues   *uint64  `json:"sum_values"`
	Buckets     []uint64 `json:"buckets"`
}

// UnmarshalJSON requires extrema/sum fields to be present so omitted keys
// cannot decode as zeros and be published as min/max/avg=0.
func (h *Raw) UnmarshalJSON(data []byte) error {
	var payload rawJSON
	if err := json.Unmarshal(data, &payload); err != nil {
		return err
	}
	if payload.MinRecorded == nil {
		return fmt.Errorf("histogram missing min_recorded")
	}
	if payload.MaxRecorded == nil {
		return fmt.Errorf("histogram missing max_recorded")
	}
	if payload.SumValues == nil {
		return fmt.Errorf("histogram missing sum_values")
	}
	*h = Raw{
		Layout:      payload.Layout,
		Unit:        payload.Unit,
		HdrTill:     payload.HdrTill,
		MaxValue:    payload.MaxValue,
		TotalCount:  payload.TotalCount,
		MinRecorded: *payload.MinRecorded,
		MaxRecorded: *payload.MaxRecorded,
		SumValues:   *payload.SumValues,
		Buckets:     payload.Buckets,
	}
	return nil
}

// ExpectedBucketCount mirrors THistogram::GetTotalBuckets for linear_exp.
// Layout is hdrTill linear buckets, then exponential buckets up to maxValue,
// then one overflow bucket for values >= maxValue.
func ExpectedBucketCount(hdrTill, maxValue uint64) (int, error) {
	if hdrTill == 0 || maxValue == 0 || hdrTill > maxValue {
		return 0, fmt.Errorf("invalid histogram parameters: hdr_till=%d max_value=%d", hdrTill, maxValue)
	}
	exp := 0
	if hdrTill <= math.MaxUint64/2 {
		for r := hdrTill * 2; r <= maxValue; {
			exp++
			if r > math.MaxUint64/2 {
				break
			}
			r *= 2
		}
	}
	return int(hdrTill) + exp + 1, nil
}

// Validate checks that a raw histogram payload is self-consistent and mergeable.
func Validate(h Raw) error {
	if h.Layout == "" {
		return fmt.Errorf("histogram missing layout")
	}
	if h.Layout != LayoutLinearExp {
		return fmt.Errorf("unsupported histogram layout %q", h.Layout)
	}
	if h.Unit == "" {
		return fmt.Errorf("histogram missing unit")
	}
	want, err := ExpectedBucketCount(h.HdrTill, h.MaxValue)
	if err != nil {
		return err
	}
	if len(h.Buckets) != want {
		return fmt.Errorf("histogram bucket length %d != expected %d for hdr_till=%d max_value=%d",
			len(h.Buckets), want, h.HdrTill, h.MaxValue)
	}
	var sum uint64
	for _, c := range h.Buckets {
		next, err := addUint64(sum, c)
		if err != nil {
			return fmt.Errorf("histogram bucket counts overflow uint64")
		}
		sum = next
	}
	if sum != h.TotalCount {
		return fmt.Errorf("histogram total_count %d != sum(buckets) %d", h.TotalCount, sum)
	}
	if h.TotalCount == 0 {
		if h.SumValues != 0 {
			return fmt.Errorf("histogram sum_values %d != 0 for empty histogram", h.SumValues)
		}
		if h.MinRecorded != 0 || h.MaxRecorded != 0 {
			return fmt.Errorf("histogram min_recorded/max_recorded must be 0 for empty histogram")
		}
		return nil
	}
	if h.MinRecorded > h.MaxRecorded {
		return fmt.Errorf("histogram min_recorded %d > max_recorded %d", h.MinRecorded, h.MaxRecorded)
	}
	if err := validateSumBounds(h); err != nil {
		return err
	}
	return validateExtremaBuckets(h)
}

func validateSumBounds(h Raw) error {
	if h.TotalCount == 1 {
		if h.MinRecorded != h.MaxRecorded || h.SumValues != h.MinRecorded {
			return fmt.Errorf("histogram single-sample min_recorded=%d max_recorded=%d sum_values=%d",
				h.MinRecorded, h.MaxRecorded, h.SumValues)
		}
		return nil
	}
	lower, err := mulUint64(h.MinRecorded, h.TotalCount-1)
	if err != nil {
		return fmt.Errorf("histogram sum_values lower bound overflows uint64")
	}
	lower, err = addUint64(lower, h.MaxRecorded)
	if err != nil {
		return fmt.Errorf("histogram sum_values lower bound overflows uint64")
	}
	if h.SumValues < lower {
		return fmt.Errorf("histogram sum_values %d below min/max bound %d", h.SumValues, lower)
	}
	if upper, err := mulUint64(h.MaxRecorded, h.TotalCount); err == nil && h.SumValues > upper {
		return fmt.Errorf("histogram sum_values %d above max_recorded*%d bound %d",
			h.SumValues, h.TotalCount, upper)
	}
	return nil
}

func validateExtremaBuckets(h Raw) error {
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
		return fmt.Errorf("histogram total_count %d but all buckets are empty", h.TotalCount)
	}
	if got := bucketIndex(h, h.MinRecorded); got != first {
		return fmt.Errorf("histogram min_recorded %d is in bucket %d, first occupied is %d",
			h.MinRecorded, got, first)
	}
	if got := bucketIndex(h, h.MaxRecorded); got != last {
		return fmt.Errorf("histogram max_recorded %d is in bucket %d, last occupied is %d",
			h.MaxRecorded, got, last)
	}
	return nil
}

// Merge sums compatible histograms (same layout / unit / hdr_till / max_value / buckets).
func Merge(dst *Raw, src Raw) error {
	if err := Validate(src); err != nil {
		return err
	}
	if dst.HdrTill == 0 && dst.MaxValue == 0 && len(dst.Buckets) == 0 && dst.Layout == "" {
		*dst = src
		cp := make([]uint64, len(src.Buckets))
		copy(cp, src.Buckets)
		dst.Buckets = cp
		return nil
	}
	if err := Validate(*dst); err != nil {
		return fmt.Errorf("destination histogram: %w", err)
	}
	if dst.HdrTill != src.HdrTill || dst.MaxValue != src.MaxValue {
		return fmt.Errorf("histogram parameter mismatch: dst(%d,%d) vs src(%d,%d)",
			dst.HdrTill, dst.MaxValue, src.HdrTill, src.MaxValue)
	}
	if dst.Layout != src.Layout {
		return fmt.Errorf("histogram layout mismatch: %s vs %s", dst.Layout, src.Layout)
	}
	if dst.Unit != src.Unit {
		return fmt.Errorf("histogram unit mismatch: %s vs %s", dst.Unit, src.Unit)
	}
	if len(dst.Buckets) != len(src.Buckets) {
		return fmt.Errorf("histogram bucket length mismatch: %d vs %d", len(dst.Buckets), len(src.Buckets))
	}
	for i := range src.Buckets {
		next, err := addUint64(dst.Buckets[i], src.Buckets[i])
		if err != nil {
			return fmt.Errorf("histogram bucket %d count overflow", i)
		}
		dst.Buckets[i] = next
	}
	total, err := addUint64(dst.TotalCount, src.TotalCount)
	if err != nil {
		return fmt.Errorf("histogram total_count overflow")
	}
	sum, err := addUint64(dst.SumValues, src.SumValues)
	if err != nil {
		return fmt.Errorf("histogram sum_values overflow")
	}
	if src.TotalCount > 0 {
		if dst.TotalCount == 0 || src.MinRecorded < dst.MinRecorded {
			dst.MinRecorded = src.MinRecorded
		}
		if dst.TotalCount == 0 || src.MaxRecorded > dst.MaxRecorded {
			dst.MaxRecorded = src.MaxRecorded
		}
	}
	dst.TotalCount = total
	dst.SumValues = sum
	return nil
}

// ValueAtPercentile mirrors THistogram::GetValueAtPercentile.
func ValueAtPercentile(h Raw, percentile float64) (uint64, error) {
	if percentile < 0 || percentile > 100 {
		return 0, fmt.Errorf("percentile must be between 0 and 100")
	}
	if h.TotalCount == 0 || len(h.Buckets) == 0 {
		return 0, nil
	}
	target := uint64(math.Ceil(percentile * float64(h.TotalCount) / 100.0))
	var current uint64
	for i, c := range h.Buckets {
		current += c
		if current >= target {
			return bucketUpperBound(h, i), nil
		}
	}
	return math.MaxUint64, nil
}

func bucketUpperBound(h Raw, bucketIndex int) uint64 {
	if uint64(bucketIndex) < h.HdrTill {
		return uint64(bucketIndex + 1)
	}
	if bucketIndex == len(h.Buckets)-1 {
		if h.MaxRecorded > 0 {
			return h.MaxRecorded
		}
		return math.MaxUint64
	}
	bucketStart := h.HdrTill
	bucketSize := h.HdrTill
	current := int(h.HdrTill)
	for current < bucketIndex {
		bucketStart += bucketSize
		bucketSize *= 2
		current++
	}
	return bucketStart + bucketSize
}

// bucketIndex mirrors THistogram::GetBucketIndex.
func bucketIndex(h Raw, value uint64) int {
	if value < h.HdrTill {
		return int(value)
	}
	bucketStart := h.HdrTill
	bucketSize := h.HdrTill
	idx := int(h.HdrTill)
	for idx < len(h.Buckets)-1 {
		bucketEnd := bucketStart + bucketSize
		if value < bucketEnd {
			return idx
		}
		bucketStart = bucketEnd
		bucketSize *= 2
		idx++
	}
	return len(h.Buckets) - 1
}

func addUint64(a, b uint64) (uint64, error) {
	if b > math.MaxUint64-a {
		return 0, fmt.Errorf("uint64 overflow")
	}
	return a + b, nil
}

func mulUint64(a, b uint64) (uint64, error) {
	if a != 0 && b > math.MaxUint64/a {
		return 0, fmt.Errorf("uint64 overflow")
	}
	return a * b, nil
}

// Percentiles returns p50/p90/p95/p99 for a merged histogram.
func Percentiles(h Raw) (map[string]uint64, error) {
	out := map[string]uint64{}
	for _, p := range []struct {
		name string
		pct  float64
	}{
		{"p50", 50},
		{"p90", 90},
		{"p95", 95},
		{"p99", 99},
	} {
		v, err := ValueAtPercentile(h, p.pct)
		if err != nil {
			return nil, err
		}
		out[p.name] = v
	}
	return out, nil
}

// ReportStats returns response-time stats for aggregate reporting:
// min/max/avg plus p50/p90/p95/p99. Average uses exact sum_values/total_count.
func ReportStats(h Raw) (map[string]interface{}, error) {
	pct, err := Percentiles(h)
	if err != nil {
		return nil, err
	}
	out := map[string]interface{}{
		"min": uint64(0),
		"max": uint64(0),
		"avg": 0.0,
		"p50": pct["p50"],
		"p90": pct["p90"],
		"p95": pct["p95"],
		"p99": pct["p99"],
	}
	if h.TotalCount == 0 {
		return out, nil
	}
	out["min"] = h.MinRecorded
	out["max"] = h.MaxRecorded
	out["avg"] = float64(h.SumValues) / float64(h.TotalCount)
	return out, nil
}

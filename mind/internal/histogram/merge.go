package histogram

import (
	"encoding/json"
	"fmt"
	"math"
	"sort"
)

const (
	LayoutLinearExp     = "linear_exp"
	SubBucketsPerOctave = 64
)

// Raw is the portable linear_exp histogram payload from worker result.json.
type Raw struct {
	Layout              string         `json:"layout"`
	Unit                string         `json:"unit"`
	HdrTill             uint64         `json:"hdr_till"`
	MaxValue            uint64         `json:"max_value"`
	SubBucketsPerOctave uint64         `json:"sub_buckets_per_octave"`
	TotalCount          uint64         `json:"total_count"`
	OverflowCount       uint64         `json:"overflow_count"`
	MinRecorded         uint64         `json:"min_recorded"`
	MaxRecorded         uint64         `json:"max_recorded"`
	SumValues           uint64         `json:"sum_values"`
	Buckets             []uint64       `json:"buckets"`
	Components          map[string]Raw `json:"components,omitempty"`
}

type rawJSON struct {
	Layout              string         `json:"layout"`
	Unit                string         `json:"unit"`
	HdrTill             uint64         `json:"hdr_till"`
	MaxValue            uint64         `json:"max_value"`
	SubBucketsPerOctave uint64         `json:"sub_buckets_per_octave"`
	TotalCount          uint64         `json:"total_count"`
	OverflowCount       *uint64        `json:"overflow_count"`
	MinRecorded         *uint64        `json:"min_recorded"`
	MaxRecorded         *uint64        `json:"max_recorded"`
	SumValues           *uint64        `json:"sum_values"`
	Buckets             []uint64       `json:"buckets"`
	Components          map[string]Raw `json:"components"`
}

// UnmarshalJSON requires extrema/sum/overflow fields to be present so omitted
// keys cannot decode as zeros and be published as min/max/avg=0.
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
	if payload.OverflowCount == nil {
		return fmt.Errorf("histogram missing overflow_count")
	}
	subBuckets := payload.SubBucketsPerOctave
	if subBuckets == 0 {
		subBuckets = SubBucketsPerOctave
	}
	*h = Raw{
		Layout:              payload.Layout,
		Unit:                payload.Unit,
		HdrTill:             payload.HdrTill,
		MaxValue:            payload.MaxValue,
		SubBucketsPerOctave: subBuckets,
		TotalCount:          payload.TotalCount,
		OverflowCount:       *payload.OverflowCount,
		MinRecorded:         *payload.MinRecorded,
		MaxRecorded:         *payload.MaxRecorded,
		SumValues:           *payload.SumValues,
		Buckets:             payload.Buckets,
		Components:          payload.Components,
	}
	return nil
}

func subBucketCount(span uint64) int {
	if span == 0 {
		return 1
	}
	if span < SubBucketsPerOctave {
		return int(span)
	}
	return SubBucketsPerOctave
}

func addSaturating(a, b uint64) uint64 {
	if b > math.MaxUint64-a {
		return math.MaxUint64
	}
	return a + b
}

type octave struct {
	start uint64
	end   uint64
	span  uint64
	nsubs int
}

func octaves(hdrTill, maxValue uint64) []octave {
	var out []octave
	start := hdrTill
	size := hdrTill
	for start < maxValue {
		end := addSaturating(start, size)
		if end > maxValue {
			end = maxValue
		}
		span := end - start
		nsubs := subBucketCount(span)
		out = append(out, octave{start: start, end: end, span: span, nsubs: nsubs})
		start = end
		if size > math.MaxUint64/2 {
			break
		}
		size *= 2
	}
	return out
}

// ExpectedBucketCount mirrors THistogram::GetTotalBuckets for linear_exp.
// Layout is hdrTill linear buckets, then at most 64 sub-buckets per doubling
// octave up to maxValue. Overflow samples are counted separately.
func ExpectedBucketCount(hdrTill, maxValue uint64) (int, error) {
	if hdrTill == 0 || maxValue == 0 || hdrTill > maxValue {
		return 0, fmt.Errorf("invalid histogram parameters: hdr_till=%d max_value=%d", hdrTill, maxValue)
	}
	n := int(hdrTill)
	for _, oct := range octaves(hdrTill, maxValue) {
		n += oct.nsubs
	}
	return n, nil
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
	if h.SubBucketsPerOctave != 0 && h.SubBucketsPerOctave != SubBucketsPerOctave {
		return fmt.Errorf("unsupported sub_buckets_per_octave %d", h.SubBucketsPerOctave)
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
	withOverflow, err := addUint64(sum, h.OverflowCount)
	if err != nil {
		return fmt.Errorf("histogram bucket counts overflow uint64")
	}
	if withOverflow != h.TotalCount {
		return fmt.Errorf("histogram total_count %d != sum(buckets)+overflow_count %d", h.TotalCount, withOverflow)
	}
	if h.TotalCount == 0 {
		if h.SumValues != 0 {
			return fmt.Errorf("histogram sum_values %d != 0 for empty histogram", h.SumValues)
		}
		if h.OverflowCount != 0 {
			return fmt.Errorf("histogram overflow_count %d != 0 for empty histogram", h.OverflowCount)
		}
		if h.MinRecorded != 0 || h.MaxRecorded != 0 {
			return fmt.Errorf("histogram min_recorded/max_recorded must be 0 for empty histogram")
		}
	} else {
		if h.MinRecorded > h.MaxRecorded {
			return fmt.Errorf("histogram min_recorded %d > max_recorded %d", h.MinRecorded, h.MaxRecorded)
		}
		if err := validateSumBounds(h); err != nil {
			return err
		}
		if err := validateExtremaBuckets(h); err != nil {
			return err
		}
	}
	if err := validateComponents(h); err != nil {
		return err
	}
	return nil
}

func validateComponents(h Raw) error {
	if len(h.Components) == 0 {
		return nil
	}
	names := make([]string, 0, len(h.Components))
	for name := range h.Components {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		comp := h.Components[name]
		if len(comp.Components) > 0 {
			return fmt.Errorf("histogram component %q must not nest further components", name)
		}
		if err := Validate(comp); err != nil {
			return fmt.Errorf("histogram component %q: %w", name, err)
		}
	}
	return nil
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
	inRange := h.TotalCount - h.OverflowCount
	if inRange == 0 {
		if first >= 0 {
			return fmt.Errorf("histogram overflow-only payload has occupied in-range buckets")
		}
		if h.MinRecorded < h.MaxValue || h.MaxRecorded < h.MaxValue {
			return fmt.Errorf("histogram overflow-only min_recorded/max_recorded must be >= max_value")
		}
		return nil
	}
	if first < 0 {
		return fmt.Errorf("histogram in-range count %d but all buckets are empty", inRange)
	}
	if got := bucketIndex(h, h.MinRecorded); got != first {
		return fmt.Errorf("histogram min_recorded %d is in bucket %d, first occupied is %d",
			h.MinRecorded, got, first)
	}
	if h.OverflowCount > 0 {
		if h.MaxRecorded < h.MaxValue {
			return fmt.Errorf("histogram overflow_count %d but max_recorded %d < max_value %d",
				h.OverflowCount, h.MaxRecorded, h.MaxValue)
		}
		return nil
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
		if src.Components != nil {
			dst.Components = cloneComponents(src.Components)
		}
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
	if subBuckets(dst) != subBuckets(&src) {
		return fmt.Errorf("histogram sub_buckets_per_octave mismatch: %d vs %d",
			subBuckets(dst), subBuckets(&src))
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
	overflow, err := addUint64(dst.OverflowCount, src.OverflowCount)
	if err != nil {
		return fmt.Errorf("histogram overflow_count overflow")
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
	dst.OverflowCount = overflow
	dst.SumValues = sum
	if err := mergeComponents(dst, src); err != nil {
		return err
	}
	return nil
}

func subBuckets(h *Raw) uint64 {
	if h.SubBucketsPerOctave == 0 {
		return SubBucketsPerOctave
	}
	return h.SubBucketsPerOctave
}

func cloneComponents(src map[string]Raw) map[string]Raw {
	out := make(map[string]Raw, len(src))
	for k, v := range src {
		cp := v
		cp.Buckets = append([]uint64(nil), v.Buckets...)
		if v.Components != nil {
			cp.Components = cloneComponents(v.Components)
		}
		out[k] = cp
	}
	return out
}

func mergeComponents(dst *Raw, src Raw) error {
	if len(src.Components) == 0 {
		return nil
	}
	if dst.Components == nil {
		dst.Components = cloneComponents(src.Components)
		return nil
	}
	names := make([]string, 0, len(src.Components))
	for name := range src.Components {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		cur := dst.Components[name]
		if err := Merge(&cur, src.Components[name]); err != nil {
			return fmt.Errorf("merge component %s: %w", name, err)
		}
		dst.Components[name] = cur
	}
	return nil
}

// ValueAtPercentile mirrors THistogram::GetValueAtPercentile.
func ValueAtPercentile(h Raw, percentile float64) (uint64, error) {
	if percentile < 0 || percentile > 100 {
		return 0, fmt.Errorf("percentile must be between 0 and 100")
	}
	if h.TotalCount == 0 {
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
	if h.OverflowCount > 0 {
		current += h.OverflowCount
		if current >= target {
			return h.MaxValue, nil
		}
	}
	return math.MaxUint64, nil
}

func bucketUpperBound(h Raw, bucketIndex int) uint64 {
	if uint64(bucketIndex) < h.HdrTill {
		return uint64(bucketIndex + 1)
	}
	index := int(h.HdrTill)
	for _, oct := range octaves(h.HdrTill, h.MaxValue) {
		if bucketIndex < index+oct.nsubs {
			sub := uint64(bucketIndex - index)
			upper := oct.start + ((sub+1)*oct.span)/uint64(oct.nsubs)
			if upper > oct.end || upper < oct.start {
				return oct.end
			}
			return upper
		}
		index += oct.nsubs
	}
	return h.MaxValue
}

// BucketForValue returns the in-range bucket index for value, or -1 if the
// sample belongs to overflow (value >= max_value).
func BucketForValue(h Raw, value uint64) int {
	return bucketIndex(h, value)
}

// bucketIndex mirrors THistogram::GetBucketIndex.
func bucketIndex(h Raw, value uint64) int {
	if value >= h.MaxValue {
		return -1
	}
	if value < h.HdrTill {
		return int(value)
	}
	index := int(h.HdrTill)
	for _, oct := range octaves(h.HdrTill, h.MaxValue) {
		if value >= oct.start && value < oct.end {
			off := value - oct.start
			sub := int((off * uint64(oct.nsubs)) / oct.span)
			if sub >= oct.nsubs {
				sub = oct.nsubs - 1
			}
			return index + sub
		}
		index += oct.nsubs
	}
	return -1
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
		"min":            uint64(0),
		"max":            uint64(0),
		"avg":            0.0,
		"p50":            pct["p50"],
		"p90":            pct["p90"],
		"p95":            pct["p95"],
		"p99":            pct["p99"],
		"overflow_count": h.OverflowCount,
	}
	if h.TotalCount == 0 {
		return attachComponentStats(out, h)
	}
	out["min"] = h.MinRecorded
	out["max"] = h.MaxRecorded
	out["avg"] = float64(h.SumValues) / float64(h.TotalCount)
	return attachComponentStats(out, h)
}

func attachComponentStats(out map[string]interface{}, h Raw) (map[string]interface{}, error) {
	if len(h.Components) == 0 {
		return out, nil
	}
	comps := map[string]interface{}{}
	names := make([]string, 0, len(h.Components))
	for name := range h.Components {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		stats, err := ReportStats(h.Components[name])
		if err != nil {
			return nil, fmt.Errorf("component %s: %w", name, err)
		}
		comps[name] = stats
	}
	out["components"] = comps
	return out, nil
}

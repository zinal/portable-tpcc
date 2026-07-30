package histogram

import (
	"fmt"
	"math"
)

// Raw is the portable linear_exp histogram payload from worker result.json.
type Raw struct {
	Layout      string   `json:"layout"`
	Unit        string   `json:"unit"`
	HdrTill     uint64   `json:"hdr_till"`
	MaxValue    uint64   `json:"max_value"`
	TotalCount  uint64   `json:"total_count"`
	MaxRecorded uint64   `json:"max_recorded"`
	Buckets     []uint64 `json:"buckets"`
}

// Merge sums compatible histograms (same hdr_till / max_value / layout).
func Merge(dst *Raw, src Raw) error {
	if dst.HdrTill == 0 && dst.MaxValue == 0 && len(dst.Buckets) == 0 {
		*dst = src
		if dst.Buckets != nil {
			cp := make([]uint64, len(src.Buckets))
			copy(cp, src.Buckets)
			dst.Buckets = cp
		}
		return nil
	}
	if dst.HdrTill != src.HdrTill || dst.MaxValue != src.MaxValue {
		return fmt.Errorf("histogram parameter mismatch: dst(%d,%d) vs src(%d,%d)",
			dst.HdrTill, dst.MaxValue, src.HdrTill, src.MaxValue)
	}
	if dst.Layout != "" && src.Layout != "" && dst.Layout != src.Layout {
		return fmt.Errorf("histogram layout mismatch: %s vs %s", dst.Layout, src.Layout)
	}
	if dst.Unit != "" && src.Unit != "" && dst.Unit != src.Unit {
		return fmt.Errorf("histogram unit mismatch: %s vs %s", dst.Unit, src.Unit)
	}
	if len(dst.Buckets) < len(src.Buckets) {
		grown := make([]uint64, len(src.Buckets))
		copy(grown, dst.Buckets)
		dst.Buckets = grown
	}
	for i := range src.Buckets {
		dst.Buckets[i] += src.Buckets[i]
	}
	dst.TotalCount += src.TotalCount
	if src.MaxRecorded > dst.MaxRecorded {
		dst.MaxRecorded = src.MaxRecorded
	}
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

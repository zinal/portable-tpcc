package consolidate

import (
	"fmt"
	"strings"

	"portable-tpcc/mind/internal/config"
)

// TPC-C 5.11 Clause 5.2.5.3 / 5.2.5.7 90th-percentile Transaction RT limits.
// Checked types match ETransactionType / summaryTxOrder. Extra histogram keys
// are ignored. A type with neither completed transactions nor p90 is skipped.
var tpccP90Limits = []struct {
	Key     string
	Name    string
	LimitMs float64
}{
	{"new_order", "NewOrder", config.TPCCP90LimitMs},
	{"delivery", "Delivery", config.TPCCP90LimitMs},
	{"order_status", "OrderStatus", config.TPCCP90LimitMs},
	{"payment", "Payment", config.TPCCP90LimitMs},
	{"stock_level", "StockLevel", config.TPCCStockLevelP90LimitMs},
}

const (
	invalidRunBannerLine = "************************************************************************"
	invalidRunBannerHead = "*** INVALID RUN: TPC-C 5.11 response-time constraints not met"
	invalidRunMarkerLine = "  *** INVALID RUN (latency constraints not met)"
)

// latencyConstraintViolations reports Clause 5.2.5.3 / 5.2.5.7 failures for
// merged measurement stats. p90 must be strictly less than the limit.
// This is an engineering indicator, not an official TPC-C verdict.
func latencyConstraintViolations(meas map[string]interface{}) []string {
	if meas == nil {
		return nil
	}
	unit, rt := responseTimeMap(meas)
	counters := counterMap(meas["counters"])
	var out []string
	for _, lim := range tpccP90Limits {
		completed := counters[lim.Key+"_ok"] + counters[lim.Key+"_user_aborted"]
		stats := rt[lim.Key]
		if completed == 0 && stats == nil {
			continue
		}
		p90Ms, ok := p90Milliseconds(stats, unit)
		if !ok {
			if completed > 0 {
				out = append(out, fmt.Sprintf("%s completed=%d without p90", lim.Name, completed))
			}
			continue
		}
		if completed == 0 && p90Ms == 0 {
			continue
		}
		if p90Ms >= lim.LimitMs {
			out = append(out, fmt.Sprintf(
				"%s p90=%s exceeds %s (Clause 5.2.5.3)",
				lim.Name,
				formatLatencyMs(p90Ms, "ms"),
				formatLatencyMs(lim.LimitMs, "ms"),
			))
		}
	}
	return out
}

func p90Milliseconds(stats map[string]interface{}, unit string) (float64, bool) {
	if stats == nil {
		return 0, false
	}
	v, ok := asFloat64(stats["p90"])
	if !ok {
		return 0, false
	}
	if unit == "us" {
		v /= 1000.0
	}
	return v, true
}

func measurementFromAggregate(agg *Aggregate) map[string]interface{} {
	if agg == nil || agg.Metrics == nil {
		return nil
	}
	meas, _ := agg.Metrics["measurement"].(map[string]interface{})
	return meas
}

func appendLatencyInvalidBanner(b *strings.Builder, violations []string) {
	if len(violations) == 0 {
		return
	}
	b.WriteString(invalidRunBannerLine)
	b.WriteByte('\n')
	b.WriteString(invalidRunBannerHead)
	b.WriteByte('\n')
	for _, v := range violations {
		fmt.Fprintf(b, "***   %s\n", v)
	}
	b.WriteString(invalidRunBannerLine)
	b.WriteByte('\n')
}

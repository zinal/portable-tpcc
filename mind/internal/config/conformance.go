package config

import (
	"fmt"
)

// TPC-C 5.11 minimum transaction mix percentages (Clause 5.2.3 / 5.2.5.7).
// New-Order has no mix minimum: its measured rate is the reported throughput.
const (
	TPCCMinMixPaymentPct     = 43
	TPCCMinMixOrderStatusPct = 4
	TPCCMinMixDeliveryPct    = 4
	TPCCMinMixStockLevelPct  = 4
)

// TPC-C 5.11 minimum measurement interval (§5.5): 120 minutes.
const TPCCMinMeasurementMs = int64(120 * 60 * 1000)

// TPCSettingsDeviations compares effective run settings against the fixed
// TPC-C 5.11 launch-parameter requirements used by portable-tpcc defaults.
// Deviations are informational: they do not reject engineering profiles.
func TPCSettingsDeviations(rc *RunConfig) []string {
	if rc == nil {
		return nil
	}
	return tpcSettingsDeviations(
		rc.Workload,
		rc.Runtime.Pacing,
		rc.Runtime.ThinkTimeDistribution,
		rc.Phases.MeasurementMs,
	)
}

func tpcSettingsDeviations(
	wl WorkloadBlock,
	pacing string,
	thinkDist string,
	measurementMs int64,
) []string {
	var out []string
	defaults := DefaultWorkload()

	if wl.TerminalsPerWarehouse != defaults.TerminalsPerWarehouse {
		out = append(out, fmt.Sprintf(
			"workload.terminals_per_warehouse=%d; TPC-C requires %d",
			wl.TerminalsPerWarehouse, defaults.TerminalsPerWarehouse,
		))
		if wl.TerminalsPerWarehouse > defaults.TerminalsPerWarehouse {
			out = append(out,
				"workload.terminals_per_warehouse>10; Stock-Level home (W_ID, D_ID) uniqueness from TPC-C §2.8.1.1 cannot hold",
			)
		}
	}

	out = append(out, mixDeviations(wl.TransactionMix)...)

	out = append(out, timingDeviations("keying_time_ms", wl.KeyingTimeMs, defaults.KeyingTimeMs)...)
	out = append(out, timingDeviations("think_time_ms", wl.ThinkTimeMs, defaults.ThinkTimeMs)...)

	if pacing == "" {
		pacing = "enabled"
	}
	if pacing != "enabled" {
		out = append(out, fmt.Sprintf(
			"runtime.pacing=%q; TPC-C requires \"enabled\"", pacing,
		))
	}

	thinkDist = ResolveThinkTimeDistribution(thinkDist)
	if thinkDist != DefaultThinkTimeDistribution {
		out = append(out, fmt.Sprintf(
			"runtime.think_time_distribution=%q; TPC-C requires %q",
			thinkDist, DefaultThinkTimeDistribution,
		))
	}

	if measurementMs < TPCCMinMeasurementMs {
		out = append(out, fmt.Sprintf(
			"phases.measurement=%dms; TPC-C requires >= 120m (%dms)",
			measurementMs, TPCCMinMeasurementMs,
		))
	}

	return out
}

func mixDeviations(m TransactionMixJSON) []string {
	type named struct {
		name string
		w    int
		min  int // 0 = no Clause 5.2.3 minimum (New-Order)
	}
	items := []named{
		{"new_order", m.NewOrder, 0},
		{"payment", m.Payment, TPCCMinMixPaymentPct},
		{"order_status", m.OrderStatus, TPCCMinMixOrderStatusPct},
		{"delivery", m.Delivery, TPCCMinMixDeliveryPct},
		{"stock_level", m.StockLevel, TPCCMinMixStockLevelPct},
	}
	sum := 0
	for _, it := range items {
		sum += it.w
	}
	if sum <= 0 {
		return nil
	}
	var out []string
	for _, it := range items {
		if it.min <= 0 {
			continue
		}
		// weight/sum*100 >= min  <=>  weight*100 >= min*sum
		if it.w*100 < it.min*sum {
			out = append(out, fmt.Sprintf(
				"workload.transaction_mix.%s weight %d/%d is below TPC-C minimum %d%%",
				it.name, it.w, sum, it.min,
			))
		}
	}
	return out
}

func timingDeviations(field string, got, want TxTimingJSON) []string {
	type named struct {
		name string
		g    int
		w    int
	}
	items := []named{
		{"new_order", got.NewOrder, want.NewOrder},
		{"payment", got.Payment, want.Payment},
		{"order_status", got.OrderStatus, want.OrderStatus},
		{"delivery", got.Delivery, want.Delivery},
		{"stock_level", got.StockLevel, want.StockLevel},
	}
	var out []string
	for _, it := range items {
		// Clause 5.2.5.2 / 5.2.5.7: keying time and mean think time are minima.
		if it.g < it.w {
			out = append(out, fmt.Sprintf(
				"workload.%s.%s=%d; TPC-C requires >= %d",
				field, it.name, it.g, it.w,
			))
		}
	}
	return out
}

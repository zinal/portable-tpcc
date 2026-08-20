package config

import "portable-tpcc/mind/internal/profile"

// Default workload parameters embedded in mind-tpcc (specification §1, §5).
func DefaultWorkload() WorkloadBlock {
	return WorkloadBlock{
		TerminalsPerWarehouse: 10,
		TransactionMix: TransactionMixJSON{
			NewOrder:    45,
			Payment:     43,
			OrderStatus: 4,
			Delivery:    4,
			StockLevel:  4,
		},
		KeyingTimeMs: TxTimingJSON{
			NewOrder:    18000,
			Payment:     3000,
			OrderStatus: 2000,
			Delivery:    2000,
			StockLevel:  2000,
		},
		ThinkTimeMs: TxTimingJSON{
			NewOrder:    12000,
			Payment:     12000,
			OrderStatus: 10000,
			Delivery:    5000,
			StockLevel:  5000,
		},
	}
}

func DefaultHistogram() HistogramJSON {
	return HistogramJSON{
		Unit:    "us",
		Highest: 120000000,
	}
}

func DefaultRetry() RetryJSON {
	return RetryJSON{
		MaxAttempts:          4,
		InitialBackoffMs:     10,
		MaxBackoffMs:         500,
		Jitter:               "full",
		RetryAmbiguousCommit: false,
	}
}

// DefaultThinkTimeDistribution is TPC-C §5.2.5.4 negative exponential.
const DefaultThinkTimeDistribution = "exponential"

// ResolveThinkTimeDistribution returns a canonical distribution name.
// Empty input yields the TPC-C default. "constant" is accepted as an alias
// for the optional compatibility mode (fixed mean think time).
func ResolveThinkTimeDistribution(value string) string {
	switch value {
	case "", "exponential":
		return DefaultThinkTimeDistribution
	case "compatibility", "constant":
		return "compatibility"
	default:
		return value
	}
}

// ValidThinkTimeDistribution reports whether value is a known distribution.
func ValidThinkTimeDistribution(value string) bool {
	switch value {
	case "", "exponential", "compatibility", "constant":
		return true
	default:
		return false
	}
}

// ValidHistogramUnit reports whether value is a known latency unit.
// Empty input means "use the built-in default".
func ValidHistogramUnit(value string) bool {
	switch value {
	case "", "ms", "us":
		return true
	default:
		return false
	}
}

// ResolveWorkload merges profile overrides onto built-in defaults.
func ResolveWorkload(w profile.Workload) WorkloadBlock {
	out := DefaultWorkload()
	if w.TerminalsPerWarehouse > 0 {
		out.TerminalsPerWarehouse = w.TerminalsPerWarehouse
	}
	applyMixOverride(&out.TransactionMix, w.TransactionMix)
	applyTimingOverride(&out.KeyingTimeMs, w.KeyingTimeMs)
	applyTimingOverride(&out.ThinkTimeMs, w.ThinkTimeMs)
	return out
}

func applyMixOverride(dst *TransactionMixJSON, src profile.TransactionMix) {
	if src.NewOrder > 0 {
		dst.NewOrder = src.NewOrder
	}
	if src.Payment > 0 {
		dst.Payment = src.Payment
	}
	if src.OrderStatus > 0 {
		dst.OrderStatus = src.OrderStatus
	}
	if src.Delivery > 0 {
		dst.Delivery = src.Delivery
	}
	if src.StockLevel > 0 {
		dst.StockLevel = src.StockLevel
	}
}

func applyTimingOverride(dst *TxTimingJSON, src profile.TxTiming) {
	if src.NewOrder > 0 {
		dst.NewOrder = src.NewOrder
	}
	if src.Payment > 0 {
		dst.Payment = src.Payment
	}
	if src.OrderStatus > 0 {
		dst.OrderStatus = src.OrderStatus
	}
	if src.Delivery > 0 {
		dst.Delivery = src.Delivery
	}
	if src.StockLevel > 0 {
		dst.StockLevel = src.StockLevel
	}
}

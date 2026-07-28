package config

import (
	"fmt"
	"sort"
	"strings"
	"time"
)

// ValidationError collects one or more profile validation failures.
type ValidationError struct {
	Messages []string
}

func (e *ValidationError) Error() string {
	if len(e.Messages) == 1 {
		return e.Messages[0]
	}
	return fmt.Sprintf("%d validation errors:\n  - %s", len(e.Messages), strings.Join(e.Messages, "\n  - "))
}

func (e *ValidationError) add(msg string) {
	e.Messages = append(e.Messages, msg)
}

// Validate checks profile rules from spec-orchestrator §10.
func Validate(r *ResolvedProfile) error {
	if r == nil {
		return fmt.Errorf("profile is nil")
	}

	var ve ValidationError

	if r.Name == "" {
		ve.add("name is required")
	}
	if r.Paths.RemoteRoot == "" {
		ve.add("paths.remote_root is required")
	}
	if r.Paths.LocalBin == "" {
		ve.add("paths.local_bin is required")
	}
	if r.Paths.LocalData == "" {
		ve.add("paths.local_data is required")
	}
	if r.DB.Host == "" {
		ve.add("db.host is required")
	}
	if r.DB.Port <= 0 || r.DB.Port > 65535 {
		ve.add("db.port must be between 1 and 65535")
	}
	if r.DB.Name == "" {
		ve.add("db.name is required")
	}
	if r.DB.User == "" {
		ve.add("db.user is required")
	}
	if r.DB.PasswordEnv == "" {
		ve.add("db.password_env is required")
	}

	validateScale(&ve, r)
	validateSchema(&ve, r)
	validateTimeouts(&ve, r)
	validateDMMode(&ve, r)
	validateTopology(&ve, r)
	validateMEE(&ve, r)
	validateCE(&ve, r)
	validateLoadShards(&ve, r)
	validatePortConflicts(&ve, r)
	validateHostsReferenced(&ve, r)

	if !r.Scale.ClientSide {
		ve.add("client_side must be true in v1")
	}

	if len(ve.Messages) > 0 {
		return &ve
	}
	return nil
}

func validateScale(ve *ValidationError, r *ResolvedProfile) {
	s := r.Scale
	if s.Customers <= 0 {
		ve.add("scale.customers must be positive")
		return
	}
	if s.Customers%1000 != 0 {
		ve.add("scale.customers must be divisible by 1000")
	}
	if s.ActiveCustomers <= 0 {
		ve.add("scale.active_customers must be positive")
	} else if s.ActiveCustomers%1000 != 0 {
		ve.add("scale.active_customers must be divisible by 1000")
	}
	if s.ScaleFactor <= 0 {
		ve.add("scale.scale_factor must be positive")
	}
	if s.InitialTradeDays <= 0 {
		ve.add("scale.initial_trade_days must be positive")
	}
	if !r.UsesStandalone() && s.DurationSec <= 0 {
		ve.add("scale.duration_sec must be positive")
	}
}

func validateSchema(ve *ValidationError, r *ResolvedProfile) {
	switch r.Schema.Mode {
	case "base", "partitioned":
	default:
		ve.add("schema.mode must be base or partitioned")
	}
	if r.Schema.Mode == "partitioned" && r.Schema.Partitions < 2 {
		ve.add("schema.partitions must be >= 2 for partitioned mode")
	}
}

func validateTimeouts(ve *ValidationError, r *ResolvedProfile) {
	t := r.Timeouts
	checkPositive := func(name string, d time.Duration) {
		if d <= 0 {
			ve.add(fmt.Sprintf("timeouts.%s must be positive", name))
		}
	}
	checkPositive("config_distribute", t.ConfigDistribute)
	checkPositive("ready", t.Ready)
	checkPositive("cleanup_wait", t.CleanupWait)
	checkPositive("ce_completion_grace", t.CECompletionGrace)
	checkPositive("mee_drain", t.MEEDrain)
	checkPositive("stop_grace", t.StopGrace)

	if r.BaseTimeLeadSec < 5 {
		ve.add("base_time_lead_sec must be >= 5")
	}
}

// ValidateBaseTimeEpochAtRun checks explicit epoch before starting processes (§9.4).
func ValidateBaseTimeEpochAtRun(epoch int64, now time.Time, t TimeoutsConfig) error {
	minEpoch := now.UTC().Unix() + int64(t.ConfigDistribute.Seconds()) + 2*int64(t.Ready.Seconds()) + 5
	if epoch < minEpoch {
		return fmt.Errorf("base_time_epoch %d is earlier than minimum %d (now + config_distribute + 2*ready + 5s)", epoch, minEpoch)
	}
	return nil
}

func validateDMMode(ve *ValidationError, r *ResolvedProfile) {
	hasStandalone := r.UsesStandalone()
	hasDM := r.DM != nil
	hasCE := len(r.CE) > 0

	if hasStandalone && (hasDM || hasCE) {
		ve.add("standalone_driver.enabled is mutually exclusive with dm and ce entries")
	}
	if !hasStandalone && !hasDM {
		ve.add("exactly one dm entry is required when standalone_driver is disabled")
	}
	if hasDM && hasStandalone {
		ve.add("cannot specify both dm and standalone_driver")
	}
	if hasStandalone {
		sd := r.StandaloneDriver
		if sd.Users <= 0 {
			ve.add("standalone_driver.users must be positive")
		}
		if sd.CEIDBase < 1 {
			ve.add("standalone_driver.ce_id_base must be >= 1")
		}
		if sd.DurationSec <= 0 {
			ve.add("standalone_driver.duration_sec must be positive")
		}
		if sd.Host == "" {
			ve.add("standalone_driver.host is required")
		}
		if sd.Output == "" {
			ve.add("standalone_driver.output is required")
		}
	}
	if hasDM {
		if r.DM.Name == "" {
			ve.add("dm.name is required")
		}
		if r.DM.Host == "" {
			ve.add("dm.host is required")
		}
		if r.DM.Output == "" {
			ve.add("dm.output is required")
		}
	}
}

func validateTopology(ve *ValidationError, r *ResolvedProfile) {
	if len(r.BH) < 1 {
		ve.add("at least one bh instance is required")
	}
	if len(r.MEE) < 1 {
		ve.add("at least one mee instance is required")
	}
	for i, bh := range r.BH {
		if bh.Name == "" {
			ve.add(fmt.Sprintf("bh[%d].name is required", i))
		}
		if bh.Host == "" {
			ve.add(fmt.Sprintf("bh[%d].host is required", i))
		}
		if bh.Listen <= 0 || bh.Listen > 65535 {
			ve.add(fmt.Sprintf("bh[%d].listen must be between 1 and 65535", i))
		}
		if bh.Output == "" {
			ve.add(fmt.Sprintf("bh[%d].output is required", i))
		}
	}
}

func validateMEE(ve *ValidationError, r *ResolvedProfile) {
	seen := make(map[int]string)
	for i, mee := range r.MEE {
		if mee.Name == "" {
			ve.add(fmt.Sprintf("mee[%d].name is required", i))
		}
		if mee.Host == "" {
			ve.add(fmt.Sprintf("mee[%d].host is required", i))
		}
		if mee.Listen <= 0 || mee.Listen > 65535 {
			ve.add(fmt.Sprintf("mee[%d].listen must be between 1 and 65535", i))
		}
		if mee.UniqueID < 1 {
			ve.add(fmt.Sprintf("mee[%d].unique_id must be >= 1", i))
		} else if prev, ok := seen[mee.UniqueID]; ok {
			ve.add(fmt.Sprintf("duplicate mee unique_id %d (%s and %s)", mee.UniqueID, prev, mee.Name))
		} else {
			seen[mee.UniqueID] = mee.Name
		}
		if mee.Output == "" {
			ve.add(fmt.Sprintf("mee[%d].output is required", i))
		}
	}
}

type ceInterval struct {
	name  string
	start int
	end   int // exclusive
}

func validateCE(ve *ValidationError, r *ResolvedProfile) {
	if r.UsesStandalone() {
		return
	}
	if len(r.CE) < 1 {
		ve.add("at least one ce instance is required when standalone_driver is disabled")
		return
	}

	withPartition := 0
	withoutPartition := 0
	var intervals []ceInterval
	var partitionRanges []struct {
		name  string
		start int
		count int
	}

	for i, ce := range r.CE {
		if ce.Name == "" {
			ve.add(fmt.Sprintf("ce[%d].name is required", i))
		}
		if ce.Host == "" {
			ve.add(fmt.Sprintf("ce[%d].host is required", i))
		}
		if ce.Users <= 0 {
			ve.add(fmt.Sprintf("ce[%d].users must be positive", i))
		}
		if ce.CEIDBase < 1 {
			ve.add(fmt.Sprintf("ce[%d].ce_id_base must be >= 1", i))
		}
		if ce.Output == "" {
			ve.add(fmt.Sprintf("ce[%d].output is required", i))
		}

		start := ce.CEIDBase
		end := ce.CEIDBase + ce.Users
		intervals = append(intervals, ceInterval{name: ce.Name, start: start, end: end})

		if ce.Partition != nil {
			withPartition++
			p := ce.Partition
			if p.Percent != 50 {
				ve.add(fmt.Sprintf("ce[%s].partition.percent must be 50 for a compliant run", ce.Name))
			}
			if p.StartID%1000 != 1 {
				ve.add(fmt.Sprintf("ce[%s].partition.start_id must satisfy start_id %% 1000 == 1", ce.Name))
			}
			if p.Count < 5000 {
				ve.add(fmt.Sprintf("ce[%s].partition.count must be >= 5000", ce.Name))
			}
			if p.Count%1000 != 0 {
				ve.add(fmt.Sprintf("ce[%s].partition.count must be divisible by 1000", ce.Name))
			}
			partitionRanges = append(partitionRanges, struct {
				name  string
				start int
				count int
			}{name: ce.Name, start: p.StartID, count: p.Count})
		} else {
			withoutPartition++
		}
	}

	if withPartition > 0 && withoutPartition > 0 {
		ve.add("mixed CE partition mode is not allowed: every CE must either omit partition or specify partition")
	}

	sort.Slice(intervals, func(i, j int) bool { return intervals[i].start < intervals[j].start })
	for i := 1; i < len(intervals); i++ {
		prev := intervals[i-1]
		cur := intervals[i]
		if cur.start < prev.end {
			ve.add(fmt.Sprintf("overlapping ce_id_base intervals: %s [%d,%d) and %s [%d,%d)",
				prev.name, prev.start, prev.end, cur.name, cur.start, cur.end))
		}
	}

	if withPartition > 0 && withoutPartition == 0 {
		validatePartitionUnion(ve, r, partitionRanges)
	}
}

func validatePartitionUnion(ve *ValidationError, r *ResolvedProfile, ranges []struct {
	name  string
	start int
	count int
}) {
	type span struct {
		name  string
		start int
		end   int
	}
	spans := make([]span, 0, len(ranges))
	for _, pr := range ranges {
		spans = append(spans, span{name: pr.name, start: pr.start, end: pr.start + pr.count})
	}
	sort.Slice(spans, func(i, j int) bool { return spans[i].start < spans[j].start })

	expectedStart := 1
	expectedEnd := r.Scale.Customers + 1
	cursor := expectedStart
	for _, s := range spans {
		if s.start != cursor {
			ve.add(fmt.Sprintf("ce partition gap or misalignment: expected next start_id %d, got %d (%s)", cursor, s.start, s.name))
			break
		}
		cursor = s.end
	}
	if cursor != expectedEnd {
		ve.add(fmt.Sprintf("ce partitions must cover exactly [1..%d]; union ends at %d", r.Scale.Customers, cursor-1))
	}
}

func validateLoadShards(ve *ValidationError, r *ResolvedProfile) {
	if len(r.Load.Shards) == 0 {
		return
	}
	total := 0
	for i, shard := range r.Load.Shards {
		if shard.Host == "" {
			ve.add(fmt.Sprintf("load.shards[%d].host is required", i))
		}
		if shard.Begin < 1 {
			ve.add(fmt.Sprintf("load.shards[%d].begin must be >= 1", i))
		}
		if shard.Count <= 0 {
			ve.add(fmt.Sprintf("load.shards[%d].count must be positive", i))
		} else if shard.Count%1000 != 0 {
			ve.add(fmt.Sprintf("load.shards[%d].count must be divisible by 1000", i))
		}
		total += shard.Count
	}
	if r.Scale.Customers > 0 && total != r.Scale.Customers {
		ve.add(fmt.Sprintf("sum of load shard counts (%d) must equal scale.customers (%d)", total, r.Scale.Customers))
	}
}

func validatePortConflicts(ve *ValidationError, r *ResolvedProfile) {
	type binding struct {
		role string
		name string
	}
	ports := make(map[string]map[int][]binding)

	add := func(host string, port int, role, name string) {
		if host == "" || port == 0 {
			return
		}
		if ports[host] == nil {
			ports[host] = make(map[int][]binding)
		}
		ports[host][port] = append(ports[host][port], binding{role: role, name: name})
	}

	for _, bh := range r.BH {
		add(bh.Host, bh.Listen, "bh", bh.Name)
	}
	for _, mee := range r.MEE {
		add(mee.Host, mee.Listen, "mee", mee.Name)
	}

	for host, hostPorts := range ports {
		for port, bindings := range hostPorts {
			if len(bindings) > 1 {
				var parts []string
				for _, b := range bindings {
					parts = append(parts, fmt.Sprintf("%s:%s", b.role, b.name))
				}
				sort.Strings(parts)
				ve.add(fmt.Sprintf("listening port %d conflicts on host %s: %s", port, host, strings.Join(parts, ", ")))
			}
		}
	}
}

func validateHostsReferenced(ve *ValidationError, r *ResolvedProfile) {
	check := func(field, host string) {
		if host == "" {
			return
		}
		if _, ok := r.HostAddresses[host]; !ok {
			ve.add(fmt.Sprintf("%s references unknown host %q", field, host))
		}
	}
	for i, bh := range r.BH {
		check(fmt.Sprintf("bh[%d].host", i), bh.Host)
	}
	for i, mee := range r.MEE {
		check(fmt.Sprintf("mee[%d].host", i), mee.Host)
	}
	if r.DM != nil {
		check("dm.host", r.DM.Host)
	}
	for i, ce := range r.CE {
		check(fmt.Sprintf("ce[%d].host", i), ce.Host)
	}
	if r.StandaloneDriver != nil && r.StandaloneDriver.Enabled {
		check("standalone_driver.host", r.StandaloneDriver.Host)
	}
	for i, shard := range r.Load.Shards {
		check(fmt.Sprintf("load.shards[%d].host", i), shard.Host)
	}
}

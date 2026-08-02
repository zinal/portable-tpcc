package validate

import (
	"fmt"
	"strings"
	"time"

	"portable-tpcc/tpccctl/internal/assignment"
	"portable-tpcc/tpccctl/internal/config"
	"portable-tpcc/tpccctl/internal/profile"
)

var allowedDBMS = map[string]bool{
	"ydb":       true,
	"pgsql":     true,
	"oceanbase": true,
}

// Result holds validation outcome.
// Structural failures set Valid=false. TPC-C settings deviations are reported
// separately and do not fail validation (engineering profiles MAY deviate).
type Result struct {
	Valid                  bool
	Errors                 []string
	TPCCSettingsConformant bool     `json:"tpcc_settings_conformant"`
	TPCCSettingsDeviations []string `json:"tpcc_settings_deviations,omitempty"`
}

func (r *Result) Add(err string) {
	r.Valid = false
	r.Errors = append(r.Errors, err)
}

// Profile validates a parsed profile (specification §10).
// Structural checks may reject the profile. TPC-C launch-parameter conformance
// is evaluated against effective (default-merged) settings and only populates
// TPCCSettingsDeviations / TPCCSettingsConformant.
func Profile(p *profile.Profile) *Result {
	res := &Result{Valid: true}

	if p.APIVersion != profile.APIVersion {
		res.Add(fmt.Sprintf("unknown apiVersion %q, want %s", p.APIVersion, profile.APIVersion))
	}
	if p.Kind != profile.Kind {
		res.Add(fmt.Sprintf("unknown kind %q, want %s", p.Kind, profile.Kind))
	}
	if p.Metadata.Name == "" {
		res.Add("metadata.name is required")
	}
	if p.SSH.User == "" {
		res.Add("ssh.user is required")
	}
	if !p.SSH.InsecureIgnore && p.SSH.KnownHosts == "" {
		res.Add("ssh.known_hosts is required unless ssh.insecure_ignore_host_key is true")
	}
	if p.SSH.ConnectTimeout != "" {
		if _, err := time.ParseDuration(p.SSH.ConnectTimeout); err != nil {
			res.Add("ssh.connect_timeout: " + err.Error())
		}
	}
	if !allowedDBMS[p.Database.DBMS] {
		res.Add(fmt.Sprintf("unknown database.dbms %q", p.Database.DBMS))
	}
	if p.Database.DBMS == "pgsql" {
		for key := range p.Database.Options {
			res.Add(fmt.Sprintf("unknown database.options.%s for dbms=pgsql", key))
		}
	}
	if p.Database.Endpoint == "" {
		res.Add("database.endpoint is required")
	}
	if p.Database.PasswordEnv == "" {
		res.Add("database.password_env is required")
	} else if looksLikeSecret(p.Database.PasswordEnv) {
		res.Add("database.password_env must be an environment variable name, not a secret literal")
	}
	if containsCredential(p.Database.Endpoint) {
		res.Add("credentials must not appear in database.endpoint")
	}
	if p.Scale.Warehouses <= 0 {
		res.Add("scale.warehouses must be positive")
	}
	if len(p.Loaders) == 0 {
		res.Add("loaders list must not be empty")
	}
	if len(p.Workers) == 0 {
		res.Add("workers list must not be empty")
	}
	if len(p.Loaders) > p.Scale.Warehouses {
		res.Add("loader count exceeds warehouse count")
	}
	if len(p.Workers) > p.Scale.Warehouses {
		res.Add("worker count exceeds warehouse count")
	}
	if p.Data.BatchRows < 0 {
		res.Add("data.batch_rows must not be negative")
	}
	if p.Runtime.Pacing != "" && p.Runtime.Pacing != "enabled" && p.Runtime.Pacing != "disabled" {
		res.Add("runtime.pacing must be \"enabled\" or \"disabled\"")
	}
	if !config.ValidThinkTimeDistribution(p.Runtime.ThinkTimeDistribution) {
		res.Add("runtime.think_time_distribution must be \"exponential\", \"compatibility\", or \"constant\"")
	}
	if p.Runtime.ThreadsPerWorker < 0 {
		res.Add("runtime.threads_per_worker must not be negative")
	}
	if p.Runtime.MaxInflightPerWorker < 0 {
		res.Add("runtime.max_inflight_per_worker must not be negative")
	}
	if p.Runtime.Retry.MaxAttempts < 0 {
		res.Add("runtime.retry.max_attempts must not be negative")
	}

	wl := config.ResolveWorkload(p.Workload)
	if err := validateMix(wl.TransactionMix); err != nil {
		res.Add(err.Error())
	}
	if wl.TerminalsPerWarehouse <= 0 {
		res.Add("workload.terminals_per_warehouse must be positive")
	}

	seenNames := map[string]bool{}
	remoteKeys := map[string]bool{}

	validateInstances(p, res, p.Loaders, "loader", seenNames, remoteKeys)
	validateInstances(p, res, p.Workers, "worker", seenNames, remoteKeys)

	if _, err := profile.ParseDurationMs(p.Phases.StartLead); err != nil {
		res.Add("phases.start_lead: " + err.Error())
	}
	if _, err := profile.ParseDurationMs(p.Phases.RampUp); err != nil {
		res.Add("phases.ramp_up: " + err.Error())
	}
	if _, err := profile.ParseDurationMs(p.Phases.Measurement); err != nil {
		res.Add("phases.measurement: " + err.Error())
	}
	if _, err := profile.ParseDurationMs(p.Phases.TransactionDrain); err != nil {
		res.Add("phases.transaction_drain: " + err.Error())
	}
	if _, err := profile.ParseDurationMs(p.Phases.StopGrace); err != nil {
		res.Add("phases.stop_grace: " + err.Error())
	}
	if p.Phases.AsyncWorkDrain != "" {
		if _, err := profile.ParseDurationMs(p.Phases.AsyncWorkDrain); err != nil {
			res.Add("phases.async_work_drain: " + err.Error())
		}
	}
	if _, err := profile.ParseDurationMs(p.Phases.MaxClockSkew); err != nil {
		res.Add("phases.max_clock_skew_ms: " + err.Error())
	}

	loadAssign, err := assignment.BuildLoaderAssignments(p.LoaderInstances(), p.Scale.Warehouses)
	if err != nil {
		res.Add("loader assignment: " + err.Error())
	} else if err := assignment.ValidateAssignment(loadAssign, p.Scale.Warehouses); err != nil {
		res.Add("loader assignment invalid: " + err.Error())
	}
	threads := p.Runtime.ThreadsPerWorker
	if threads <= 0 {
		threads = 1
	}
	maxInflight := p.Runtime.MaxInflightPerWorker
	if maxInflight <= 0 {
		maxInflight = 64
	}
	_, err = assignment.BuildWorkerAssignments(
		p.WorkerInstances(),
		p.Scale.Warehouses,
		threads,
		maxInflight,
	)
	if err != nil {
		res.Add("worker assignment: " + err.Error())
	}

	if p.Raw != nil {
		for _, key := range []string{"warehouse_ranges", "assignment", "owns_global_data"} {
			if hasNestedKey(p.Raw, key) {
				res.Add(fmt.Sprintf("manual %q in profile is prohibited", key))
			}
		}
		for _, key := range []string{"mode", "spec", "deviations"} {
			if _, ok := p.Raw[key]; ok {
				res.Add(fmt.Sprintf("obsolete profile field %q is not accepted", key))
			}
		}
	}

	attachTPCSettingsConformance(p, res)
	return res
}

func attachTPCSettingsConformance(p *profile.Profile, res *Result) {
	pacing := p.Runtime.Pacing
	if pacing == "" {
		pacing = "enabled"
	}
	measurementMs, err := profile.ParseDurationMs(p.Phases.Measurement)
	if err != nil {
		measurementMs = 0
	}
	devs := config.TPCSettingsDeviations(&config.RunConfig{
		Workload: config.ResolveWorkload(p.Workload),
		Phases:   config.PhasesJSON{MeasurementMs: measurementMs},
		Runtime: config.RunRuntime{
			Pacing:                pacing,
			ThinkTimeDistribution: config.ResolveThinkTimeDistribution(p.Runtime.ThinkTimeDistribution),
		},
	})
	res.TPCCSettingsDeviations = devs
	res.TPCCSettingsConformant = len(devs) == 0
}

func validateMix(m config.TransactionMixJSON) error {
	weights := []int{m.NewOrder, m.Payment, m.OrderStatus, m.Delivery, m.StockLevel}
	sum := 0
	for _, w := range weights {
		if w <= 0 {
			return fmt.Errorf("invalid mix: all transaction weights must be positive")
		}
		sum += w
	}
	if sum <= 0 {
		return fmt.Errorf("invalid mix: weights must form a complete distribution")
	}
	return nil
}

func validateInstances(
	p *profile.Profile,
	res *Result,
	items []profile.NamedHost,
	role string,
	seenNames map[string]bool,
	remoteKeys map[string]bool,
) {
	for _, item := range items {
		if err := profile.ValidateInstanceName(item.Name); err != nil {
			res.Add(fmt.Sprintf("%s %s: %v", role, item.Name, err))
		}
		if seenNames[item.Name] {
			res.Add(fmt.Sprintf("duplicate instance name %q", item.Name))
		}
		seenNames[item.Name] = true
		if item.Host == "" {
			res.Add(fmt.Sprintf("%s %s: host is required", role, item.Name))
		}
		if _, ok := p.Hosts[item.Host]; !ok {
			res.Add(fmt.Sprintf("%s %s: unknown host %q", role, item.Name, item.Host))
		}
		key := fmt.Sprintf("%s:%s:%s", p.Hosts[item.Host].Address, p.Paths.RemoteRoot, item.Name)
		if remoteKeys[key] {
			res.Add(fmt.Sprintf("duplicate remote (host, run_dir, instance): %s", key))
		}
		remoteKeys[key] = true
	}
}

func looksLikeSecret(s string) bool {
	if strings.Contains(s, "=") || (strings.Contains(s, ":") && strings.Contains(s, "@")) {
		return true
	}
	return false
}

func containsCredential(endpoint string) bool {
	lower := strings.ToLower(endpoint)
	return strings.Contains(lower, "password=") || strings.Contains(lower, "user=")
}

func hasNestedKey(m map[string]interface{}, key string) bool {
	for k, v := range m {
		if k == key {
			return true
		}
		if sub, ok := v.(map[string]interface{}); ok && hasNestedKey(sub, key) {
			return true
		}
		if arr, ok := v.([]interface{}); ok {
			for _, elem := range arr {
				if sub, ok := elem.(map[string]interface{}); ok && hasNestedKey(sub, key) {
					return true
				}
			}
		}
	}
	return false
}

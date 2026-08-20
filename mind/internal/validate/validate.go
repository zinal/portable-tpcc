package validate

import (
	"fmt"
	"strings"
	"time"

	"portable-tpcc/mind/internal/assignment"
	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/paths"
	"portable-tpcc/mind/internal/profile"
	"portable-tpcc/mind/internal/remote"
)

var allowedYdbAuthSchemes = map[string]bool{
	"anonymous": true,
	"login":     true,
	"sa_key":    true,
}

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
		validatePgsqlOptions(p.Database.Options, res)
	}
	if p.Database.DBMS == "oceanbase" {
		validateOceanbaseOptions(p.Database.Options, res)
	}
	if p.Database.Endpoint == "" {
		res.Add("database.endpoint is required")
	}
	if containsCredential(p.Database.Endpoint) {
		res.Add("credentials must not appear in database.endpoint")
	}
	validateDatabaseAuth(p, res)
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
	if p.Runtime.ThreadsPerLoader < 0 {
		res.Add("runtime.threads_per_loader must not be negative")
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

	loadAssign, err := assignment.BuildLoaderAssignments(
		p.LoaderInstances(), p.Scale.Warehouses, p.Runtime.ThreadsPerLoader)
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
			res.Add(fmt.Sprintf("%s %s: host is required (connection address)", role, item.Name))
		}
		// Identical host addresses mean co-location (one SSH/local session).
		key := fmt.Sprintf("%s:%s:%s", item.Host, p.Paths.RemoteRoot, item.Name)
		if remoteKeys[key] {
			res.Add(fmt.Sprintf("duplicate remote (host, run_dir, instance): %s", key))
		}
		remoteKeys[key] = true
	}
}

func validatePgsqlOptions(options map[string]interface{}, res *Result) {
	for key, value := range options {
		switch key {
		case "partitioning":
			s, ok := value.(string)
			if !ok || (s != "none" && s != "warehouse_hash") {
				res.Add(`database.options.partitioning must be "none" or "warehouse_hash"`)
			}
		case "partition_count":
			n, ok := asPositiveInt(value)
			if !ok {
				res.Add("database.options.partition_count must be a positive integer")
			} else if n > 1024 {
				res.Add("database.options.partition_count must not exceed 1024")
			}
		case "foreign_keys":
			if !asForeignKeysOption(value) {
				res.Add(`database.options.foreign_keys must be a boolean or "on"/"off"`)
			}
		default:
			res.Add(fmt.Sprintf("unknown database.options.%s for dbms=pgsql", key))
		}
	}
	if partitioning, has := options["partitioning"]; has {
		if s, ok := partitioning.(string); ok && s == "none" {
			if _, hasCount := options["partition_count"]; hasCount {
				res.Add("database.options.partition_count is only valid when partitioning=warehouse_hash")
			}
		}
	} else if _, hasCount := options["partition_count"]; hasCount {
		res.Add("database.options.partition_count is only valid when partitioning=warehouse_hash")
	}
}

func validateOceanbaseOptions(options map[string]interface{}, res *Result) {
	for key, value := range options {
		switch key {
		case "partitions":
			n, ok := asInt(value)
			if !ok {
				res.Add("database.options.partitions must be an integer")
			} else if n < -1 {
				res.Add("database.options.partitions must be -1, 0, or a positive integer")
			} else if n > 8192 {
				res.Add("database.options.partitions must not exceed 8192")
			}
		case "foreign_keys":
			if !asForeignKeysOption(value) {
				res.Add(`database.options.foreign_keys must be a boolean or "on"/"off"`)
			}
		case "query_timeout":
			if _, ok := asPositiveInt(value); !ok {
				res.Add("database.options.query_timeout must be a positive integer (seconds)")
			}
		case "index_parallel":
			if _, ok := asPositiveInt(value); !ok {
				res.Add("database.options.index_parallel must be a positive integer")
			}
		default:
			res.Add(fmt.Sprintf("unknown database.options.%s for dbms=oceanbase", key))
		}
	}
}

func asForeignKeysOption(value interface{}) bool {
	switch v := value.(type) {
	case bool:
		return true
	case string:
		switch v {
		case "on", "off", "true", "false", "1", "0":
			return true
		default:
			return false
		}
	default:
		return false
	}
}

func asPositiveInt(value interface{}) (int, bool) {
	n, ok := asInt(value)
	if !ok || n <= 0 {
		return 0, false
	}
	return n, true
}

func asInt(value interface{}) (int, bool) {
	switch v := value.(type) {
	case int:
		return v, true
	case int32:
		return int(v), true
	case int64:
		return int(v), true
	case uint:
		if v > uint(^uint(0)>>1) {
			return 0, false
		}
		return int(v), true
	case uint32:
		return int(v), true
	case uint64:
		if v > uint64(^uint(0)>>1) {
			return 0, false
		}
		return int(v), true
	case float64:
		if v != float64(int(v)) {
			return 0, false
		}
		return int(v), true
	default:
		return 0, false
	}
}

func validateDatabaseAuth(p *profile.Profile, res *Result) {
	db := p.Database
	switch db.DBMS {
	case "ydb":
		validateYdbAuth(db, res)
	case "oceanbase":
		if db.AuthScheme != "" {
			res.Add("database.auth_scheme is only supported for dbms=ydb")
		}
		if db.SaKeyFile != "" {
			res.Add("database.sa_key_file is only supported for dbms=ydb")
		}
		if db.CaFile != "" {
			res.Add("database.ca_file is only supported for dbms=ydb")
		}
		validatePasswordEnvRequired(db.PasswordEnv, res)
	default:
		if db.AuthScheme != "" {
			res.Add("database.auth_scheme is only supported for dbms=ydb")
		}
		if db.User != "" {
			res.Add("database.user is only supported for dbms=ydb or dbms=oceanbase")
		}
		if db.SaKeyFile != "" {
			res.Add("database.sa_key_file is only supported for dbms=ydb")
		}
		if db.CaFile != "" {
			res.Add("database.ca_file is only supported for dbms=ydb")
		}
		validatePasswordEnvRequired(db.PasswordEnv, res)
	}
}

func validateYdbAuth(db profile.Database, res *Result) {
	scheme := config.InferYdbAuthScheme(db)
	if db.AuthScheme != "" && !allowedYdbAuthSchemes[db.AuthScheme] {
		res.Add(`database.auth_scheme must be "anonymous", "login", or "sa_key"`)
		return
	}
	if db.CaFile != "" {
		if _, err := paths.ExpandHome(db.CaFile); err != nil {
			res.Add("database.ca_file: " + err.Error())
		}
	}
	switch scheme {
	case "anonymous":
		if db.User != "" || db.PasswordEnv != "" || db.SaKeyFile != "" {
			res.Add("database.auth_scheme=anonymous does not accept user, password_env, or sa_key_file")
		}
	case "login":
		if db.User == "" {
			res.Add("database.user is required for auth_scheme=login")
		}
		validatePasswordEnvRequired(db.PasswordEnv, res)
		if db.SaKeyFile != "" {
			res.Add("database.auth_scheme=login does not accept sa_key_file")
		}
	case "sa_key":
		if db.SaKeyFile == "" {
			res.Add("database.sa_key_file is required for auth_scheme=sa_key")
		} else if _, err := paths.ExpandHome(db.SaKeyFile); err != nil {
			res.Add("database.sa_key_file: " + err.Error())
		}
		if db.User != "" || db.PasswordEnv != "" {
			res.Add("database.auth_scheme=sa_key does not accept user or password_env")
		}
	}
}

func validatePasswordEnvRequired(name string, res *Result) {
	if name == "" {
		res.Add("database.password_env is required")
	} else if looksLikeSecret(name) {
		res.Add("database.password_env must be an environment variable name, not a secret literal")
	} else if !remote.ValidEnvName(name) {
		res.Add("database.password_env must match [A-Za-z_][A-Za-z0-9_]*")
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

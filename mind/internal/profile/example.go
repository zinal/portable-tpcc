package profile

import (
	"bytes"
	"fmt"
	"os/user"
	"path/filepath"
	"strconv"
	"strings"

	"gopkg.in/yaml.v3"
)

// Built-in example-profile defaults. Workload / retry / histogram numbers
// MUST match mind/internal/config defaults (specification §1, §5).
const (
	DefaultHost                        = "localhost"
	DefaultWarehouses                  = 10
	DefaultBatchRows                   = 10000
	DefaultSeed                  int64 = 1
	DefaultSSHKnownHosts               = "~/.ssh/known_hosts"
	DefaultSSHConnectTimeout           = "10s"
	DefaultLocalArtifacts              = "."
	DefaultRemoteRoot                  = "portable-tpcc"
	DefaultResultRoot                  = "results"
	DefaultStateDir                    = "state"
	DefaultStartLead                   = "45s"
	DefaultRampUp                      = "5m"
	DefaultMeasurement                 = "120m"
	DefaultTransactionDrain            = "30s"
	DefaultStopGrace                   = "15s"
	DefaultMaxClockSkew                = "100ms"
	DefaultPacing                      = "enabled"
	DefaultThinkTimeDistribution       = "exponential"
	DefaultRetryMaxAttempts            = 4
	DefaultRetryInitialBackoff         = "10ms"
	DefaultRetryMaxBackoff             = "500ms"
	DefaultRetryJitter                 = "full"
	DefaultHistogramUnit               = "us"
	DefaultHistogramHighest      int64 = 120000000
	DefaultMaxInflight                 = 100
	DefaultPasswordEnv                 = "TPCC_PASSWORD"
	DefaultYDBPasswordEnv              = "YDB_PASSWORD"
	DefaultPgUser                      = "postgres"
	DefaultPgDatabase                  = "tpcc"
	DefaultPgPath                      = "public"
	DefaultPgEndpoint                  = "localhost:5432"
	DefaultYDBUser                     = "root"
	DefaultYDBDatabase                 = "/local"
	DefaultYDBPath                     = "tpcc"
	DefaultYDBEndpoint                 = "localhost:2136"
	DefaultYDBAuthScheme               = "anonymous"
	DefaultOBUser                      = "root@root"
	DefaultOBDatabase                  = "tpcc"
	DefaultOBPath                      = "tpcc"
	DefaultOBEndpoint                  = "localhost:2881"
	DefaultPgPartitioning              = "none"
	DefaultForeignKeys                 = "on"
	DefaultOBPartitions                = 0
	DefaultOBQueryTimeout              = 600
	DefaultOBIndexParallel             = 4
	DefaultTerminalsPerWarehouse       = 10
	DefaultSSHUserFallback             = "tpcc"
	DefaultProfileName                 = "tpcc"
)

// AllowedDBMS are the database.dbms values accepted by configure.
var AllowedDBMS = map[string]bool{
	"pgsql":     true,
	"ydb":       true,
	"oceanbase": true,
}

// DefaultSSHUser returns the current account name, or "tpcc" if unknown.
func DefaultSSHUser() string {
	u, err := user.Current()
	if err != nil || u == nil || strings.TrimSpace(u.Username) == "" {
		return DefaultSSHUserFallback
	}
	return u.Username
}

// NameFromProfilePath derives metadata.name from a profile filename.
func NameFromProfilePath(path string) string {
	base := filepath.Base(strings.TrimSpace(path))
	if base == "" || base == "." || base == string(filepath.Separator) {
		return DefaultProfileName
	}
	if ext := filepath.Ext(base); ext != "" {
		base = strings.TrimSuffix(base, ext)
	}
	return sanitizeProfileName(base)
}

func sanitizeProfileName(name string) string {
	name = strings.ToLower(strings.TrimSpace(name))
	var b strings.Builder
	lastDash := false
	for _, r := range name {
		switch {
		case r >= 'a' && r <= 'z', r >= '0' && r <= '9':
			b.WriteRune(r)
			lastDash = false
		default:
			if !lastDash && b.Len() > 0 {
				b.WriteByte('-')
				lastDash = true
			}
		}
	}
	s := strings.Trim(b.String(), "-")
	if s == "" {
		return DefaultProfileName
	}
	if s[0] >= '0' && s[0] <= '9' {
		s = "p-" + s
	}
	return s
}

// Example returns a complete portable-tpcc/v1 profile with every field set
// to the built-in default for dbms, including DBMS-specific database keys.
func Example(dbms string) (*Profile, error) {
	return ExampleWithName(dbms, DefaultProfileName, DefaultSSHUser())
}

// ExampleWithName is Example with an explicit metadata.name and ssh.user.
func ExampleWithName(dbms, name, sshUser string) (*Profile, error) {
	if !AllowedDBMS[dbms] {
		return nil, fmt.Errorf("unknown database.dbms %q (want pgsql, ydb, or oceanbase)", dbms)
	}
	if name == "" {
		name = DefaultProfileName
	}
	if sshUser == "" {
		sshUser = DefaultSSHUser()
	}
	seed := DefaultSeed
	highest := DefaultHistogramHighest
	p := &Profile{
		APIVersion: APIVersion,
		Kind:       Kind,
		Metadata:   Metadata{Name: name},
		SSH: SSHConfig{
			User:           sshUser,
			UseAgent:       false,
			KnownHosts:     DefaultSSHKnownHosts,
			ConnectTimeout: DefaultSSHConnectTimeout,
			InsecureIgnore: false,
		},
		Paths: Paths{
			LocalArtifacts: DefaultLocalArtifacts,
			RemoteRoot:     DefaultRemoteRoot,
			ResultRoot:     DefaultResultRoot,
			StateDir:       DefaultStateDir,
		},
		Scale: Scale{Warehouses: DefaultWarehouses},
		Data: Data{
			Seed:      &seed,
			BatchRows: DefaultBatchRows,
		},
		Workload: Workload{
			TerminalsPerWarehouse: DefaultTerminalsPerWarehouse,
			TransactionMix: TransactionMix{
				NewOrder:    45,
				Payment:     43,
				OrderStatus: 4,
				Delivery:    4,
				StockLevel:  4,
			},
			KeyingTimeMs: TxTiming{
				NewOrder:    18000,
				Payment:     3000,
				OrderStatus: 2000,
				Delivery:    2000,
				StockLevel:  2000,
			},
			ThinkTimeMs: TxTiming{
				NewOrder:    12000,
				Payment:     12000,
				OrderStatus: 10000,
				Delivery:    5000,
				StockLevel:  5000,
			},
		},
		Loaders: []NamedHost{{Host: DefaultHost}},
		Workers: []NamedHost{{Host: DefaultHost}},
		Phases: Phases{
			StartLead:        DefaultStartLead,
			RampUp:           DefaultRampUp,
			Measurement:      DefaultMeasurement,
			TransactionDrain: DefaultTransactionDrain,
			AsyncWorkDrain:   DefaultTransactionDrain,
			StopGrace:        DefaultStopGrace,
			MaxClockSkew:     DefaultMaxClockSkew,
		},
		Runtime: Runtime{
			Pacing:                DefaultPacing,
			ThinkTimeDistribution: DefaultThinkTimeDistribution,
			ThreadsPerLoader:      0,
			ThreadsPerWorker:      0,
			MaxInflightPerWorker:  DefaultMaxInflight,
			CheckConcurrency:      0,
			Retry: RetryPolicy{
				MaxAttempts:    DefaultRetryMaxAttempts,
				InitialBackoff: DefaultRetryInitialBackoff,
				MaxBackoff:     DefaultRetryMaxBackoff,
				Jitter:         DefaultRetryJitter,
			},
			Histogram: Histogram{
				Unit:    DefaultHistogramUnit,
				Highest: &highest,
			},
		},
		Checks: Checks{
			AfterImport: false,
			AfterTest:   false,
			FailFast:    false,
		},
		Collect: Collect{
			IncludeEvents: false,
			IncludeLogs:   false,
		},
	}
	switch dbms {
	case "pgsql":
		p.Database = Database{
			DBMS:        "pgsql",
			Endpoint:    DefaultPgEndpoint,
			Database:    DefaultPgDatabase,
			Path:        DefaultPgPath,
			User:        DefaultPgUser,
			PasswordEnv: DefaultPasswordEnv,
			Options: map[string]interface{}{
				"partitioning": DefaultPgPartitioning,
				"foreign_keys": DefaultForeignKeys,
			},
		}
	case "ydb":
		p.Database = Database{
			DBMS:       "ydb",
			Endpoint:   DefaultYDBEndpoint,
			Database:   DefaultYDBDatabase,
			Path:       DefaultYDBPath,
			AuthScheme: DefaultYDBAuthScheme,
		}
	case "oceanbase":
		p.Database = Database{
			DBMS:        "oceanbase",
			Endpoint:    DefaultOBEndpoint,
			Database:    DefaultOBDatabase,
			Path:        DefaultOBPath,
			User:        DefaultOBUser,
			PasswordEnv: DefaultPasswordEnv,
			Options: map[string]interface{}{
				"partitions":     DefaultOBPartitions,
				"foreign_keys":   DefaultForeignKeys,
				"query_timeout":  DefaultOBQueryTimeout,
				"index_parallel": DefaultOBIndexParallel,
			},
		}
	}
	return p, nil
}

// HostsFromStrings builds loader/worker entries from address strings.
func HostsFromStrings(addrs []string) []NamedHost {
	out := make([]NamedHost, 0, len(addrs))
	for _, a := range addrs {
		a = strings.TrimSpace(a)
		if a == "" {
			continue
		}
		out = append(out, NamedHost{Host: a})
	}
	return out
}

type exampleDoc struct {
	APIVersion string          `yaml:"apiVersion"`
	Kind       string          `yaml:"kind"`
	Metadata   Metadata        `yaml:"metadata"`
	SSH        SSHConfig       `yaml:"ssh"`
	Paths      Paths           `yaml:"paths"`
	Database   exampleDatabase `yaml:"database"`
	Scale      Scale           `yaml:"scale"`
	Data       exampleData     `yaml:"data"`
	Workload   Workload        `yaml:"workload"`
	Loaders    []string        `yaml:"loaders"`
	Workers    []string        `yaml:"workers"`
	Phases     Phases          `yaml:"phases"`
	Runtime    exampleRuntime  `yaml:"runtime"`
	Checks     exampleChecks   `yaml:"checks"`
	Collect    Collect         `yaml:"collect"`
}

type exampleDatabase struct {
	DBMS        string     `yaml:"dbms"`
	Endpoint    string     `yaml:"endpoint"`
	Database    string     `yaml:"database"`
	Path        string     `yaml:"path"`
	AuthScheme  string     `yaml:"auth_scheme,omitempty"`
	User        string     `yaml:"user,omitempty"`
	PasswordEnv string     `yaml:"password_env,omitempty"`
	SaKeyFile   string     `yaml:"sa_key_file,omitempty"`
	CaFile      string     `yaml:"ca_file,omitempty"`
	Options     *yaml.Node `yaml:"options,omitempty"`
}

type exampleData struct {
	Seed      int64 `yaml:"seed"`
	BatchRows int   `yaml:"batch_rows"`
}

type exampleRuntime struct {
	Pacing                string           `yaml:"pacing"`
	ThinkTimeDistribution string           `yaml:"think_time_distribution"`
	ThreadsPerLoader      int              `yaml:"threads_per_loader"`
	ThreadsPerWorker      int              `yaml:"threads_per_worker"`
	CheckConcurrency      int              `yaml:"check_concurrency"`
	MaxInflightPerWorker  int              `yaml:"max_inflight_per_worker"`
	Retry                 RetryPolicy      `yaml:"retry"`
	Histogram             exampleHistogram `yaml:"histogram"`
}

type exampleHistogram struct {
	Unit    string `yaml:"unit"`
	Highest int64  `yaml:"highest"`
}

type exampleChecks struct {
	AfterImport bool `yaml:"after_import"`
	AfterTest   bool `yaml:"after_test"`
	FailFast    bool `yaml:"fail_fast"`
}

// EncodeExample renders a complete profile YAML with a stable field order.
func EncodeExample(p *Profile) ([]byte, error) {
	if p == nil {
		return nil, fmt.Errorf("nil profile")
	}
	seed := DefaultSeed
	if p.Data.Seed != nil {
		seed = *p.Data.Seed
	}
	highest := DefaultHistogramHighest
	if p.Runtime.Histogram.Highest != nil {
		highest = *p.Runtime.Histogram.Highest
	}
	doc := exampleDoc{
		APIVersion: p.APIVersion,
		Kind:       p.Kind,
		Metadata:   p.Metadata,
		SSH:        p.SSH,
		Paths:      p.Paths,
		Database: exampleDatabase{
			DBMS:        p.Database.DBMS,
			Endpoint:    p.Database.Endpoint,
			Database:    p.Database.Database,
			Path:        p.Database.Path,
			AuthScheme:  p.Database.AuthScheme,
			User:        p.Database.User,
			PasswordEnv: p.Database.PasswordEnv,
			SaKeyFile:   p.Database.SaKeyFile,
			CaFile:      p.Database.CaFile,
			Options:     encodeOptions(p.Database.DBMS, p.Database.Options),
		},
		Scale: p.Scale,
		Data: exampleData{
			Seed:      seed,
			BatchRows: p.Data.BatchRows,
		},
		Workload: p.Workload,
		Loaders:  hostStrings(p.Loaders),
		Workers:  hostStrings(p.Workers),
		Phases:   p.Phases,
		Runtime: exampleRuntime{
			Pacing:                p.Runtime.Pacing,
			ThinkTimeDistribution: p.Runtime.ThinkTimeDistribution,
			ThreadsPerLoader:      p.Runtime.ThreadsPerLoader,
			ThreadsPerWorker:      p.Runtime.ThreadsPerWorker,
			CheckConcurrency:      p.Runtime.CheckConcurrency,
			MaxInflightPerWorker:  p.Runtime.MaxInflightPerWorker,
			Retry:                 p.Runtime.Retry,
			Histogram: exampleHistogram{
				Unit:    p.Runtime.Histogram.Unit,
				Highest: highest,
			},
		},
		Checks: exampleChecks{
			AfterImport: p.Checks.AfterImport,
			AfterTest:   p.Checks.AfterTest,
			FailFast:    p.Checks.FailFast,
		},
		Collect: p.Collect,
	}

	var buf bytes.Buffer
	buf.WriteString("# Generated by mind-tpcc configure. All fields are set to built-in defaults.\n")
	enc := yaml.NewEncoder(&buf)
	enc.SetIndent(2)
	if err := enc.Encode(&doc); err != nil {
		return nil, err
	}
	if err := enc.Close(); err != nil {
		return nil, err
	}
	return []byte(withSectionBreaks(buf.String())), nil
}

func hostStrings(items []NamedHost) []string {
	out := make([]string, len(items))
	for i, h := range items {
		out[i] = h.Host
	}
	return out
}

func encodeOptions(dbms string, options map[string]interface{}) *yaml.Node {
	if len(options) == 0 {
		return nil
	}
	var keys []string
	switch dbms {
	case "pgsql":
		keys = []string{"partitioning", "foreign_keys", "partition_count"}
	case "oceanbase":
		keys = []string{"partitions", "foreign_keys", "query_timeout", "index_parallel"}
	default:
		for k := range options {
			keys = append(keys, k)
		}
	}
	n := &yaml.Node{Kind: yaml.MappingNode, Tag: "!!map"}
	for _, key := range keys {
		val, ok := options[key]
		if !ok {
			continue
		}
		n.Content = append(n.Content,
			&yaml.Node{Kind: yaml.ScalarNode, Tag: "!!str", Value: key},
			yamlScalar(val),
		)
	}
	if len(n.Content) == 0 {
		return nil
	}
	return n
}

func yamlScalar(v interface{}) *yaml.Node {
	switch x := v.(type) {
	case nil:
		return &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!null", Value: "null"}
	case bool:
		return &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!bool", Value: strconv.FormatBool(x)}
	case string:
		return &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!str", Value: x}
	case int:
		return &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!int", Value: strconv.Itoa(x)}
	case int32:
		return &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!int", Value: strconv.FormatInt(int64(x), 10)}
	case int64:
		return &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!int", Value: strconv.FormatInt(x, 10)}
	case uint:
		return &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!int", Value: strconv.FormatUint(uint64(x), 10)}
	case float64:
		if x == float64(int64(x)) {
			return &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!int", Value: strconv.FormatInt(int64(x), 10)}
		}
		return &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!float", Value: strconv.FormatFloat(x, 'g', -1, 64)}
	default:
		return &yaml.Node{Kind: yaml.ScalarNode, Value: fmt.Sprint(v)}
	}
}

func withSectionBreaks(s string) string {
	sections := []string{
		"metadata:",
		"ssh:",
		"paths:",
		"database:",
		"scale:",
		"data:",
		"workload:",
		"loaders:",
		"workers:",
		"phases:",
		"runtime:",
		"checks:",
		"collect:",
	}
	lines := strings.Split(strings.TrimSuffix(s, "\n"), "\n")
	var out []string
	for _, line := range lines {
		for _, sec := range sections {
			if line == sec && len(out) > 0 && out[len(out)-1] != "" {
				out = append(out, "")
				break
			}
		}
		out = append(out, line)
	}
	return strings.Join(out, "\n") + "\n"
}

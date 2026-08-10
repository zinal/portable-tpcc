package profile

import (
	"bytes"
	"fmt"
	"os"
	"regexp"
	"time"

	"gopkg.in/yaml.v3"
	"portable-tpcc/mind/internal/assignment"
)

const (
	APIVersion = "portable-tpcc/v1"
	Kind       = "TpccRunProfile"
)

var instanceNameRE = regexp.MustCompile(`^[a-z][a-z0-9-]*$`)

// Profile is the portable-tpcc/v1 run profile.
type Profile struct {
	APIVersion string      `yaml:"apiVersion"`
	Kind       string      `yaml:"kind"`
	Metadata   Metadata    `yaml:"metadata"`
	SSH        SSHConfig   `yaml:"ssh"`
	Hosts      Hosts       `yaml:"hosts"`
	Paths      Paths       `yaml:"paths"`
	Database   Database    `yaml:"database"`
	Scale      Scale       `yaml:"scale"`
	Data       Data        `yaml:"data"`
	Workload   Workload    `yaml:"workload"`
	Loaders    []NamedHost `yaml:"loaders"`
	Workers    []NamedHost `yaml:"workers"`
	Phases     Phases      `yaml:"phases"`
	Runtime    Runtime     `yaml:"runtime"`
	Checks     Checks      `yaml:"checks"`
	Collect    Collect     `yaml:"collect"`

	// Reject unknown fields at parse time via strict decoding.
	Raw map[string]interface{} `yaml:"-"`
}

type Metadata struct {
	Name string `yaml:"name"`
}

type SSHConfig struct {
	User           string `yaml:"user"`
	UseAgent       bool   `yaml:"use_agent"`
	KnownHosts     string `yaml:"known_hosts"`
	ConnectTimeout string `yaml:"connect_timeout"`
	InsecureIgnore bool   `yaml:"insecure_ignore_host_key"`
}

type Hosts map[string]HostEntry

type HostEntry struct {
	Address string `yaml:"address"`
}

type Paths struct {
	LocalArtifacts string `yaml:"local_artifacts"`
	RemoteRoot     string `yaml:"remote_root"`
	ResultRoot     string `yaml:"result_root"`
	StateDir       string `yaml:"state_dir"`
}

type Database struct {
	DBMS        string                 `yaml:"dbms"`
	Endpoint    string                 `yaml:"endpoint"`
	Database    string                 `yaml:"database"`
	Path        string                 `yaml:"path"`
	AuthScheme  string                 `yaml:"auth_scheme"`
	User        string                 `yaml:"user"`
	PasswordEnv string                 `yaml:"password_env"`
	SaKeyFile   string                 `yaml:"sa_key_file"`
	CaFile      string                 `yaml:"ca_file"`
	Options     map[string]interface{} `yaml:"options"`
}

type Scale struct {
	Warehouses int `yaml:"warehouses"`
}

type Data struct {
	Seed      *int64 `yaml:"seed"`
	BatchRows int    `yaml:"batch_rows"`
}

// Workload holds optional overrides; omitted fields use mind-tpcc defaults.
type Workload struct {
	TerminalsPerWarehouse int            `yaml:"terminals_per_warehouse"`
	TransactionMix        TransactionMix `yaml:"transaction_mix"`
	KeyingTimeMs          TxTiming       `yaml:"keying_time_ms"`
	ThinkTimeMs           TxTiming       `yaml:"think_time_ms"`
}

type TransactionMix struct {
	NewOrder    int `yaml:"new_order"`
	Payment     int `yaml:"payment"`
	OrderStatus int `yaml:"order_status"`
	Delivery    int `yaml:"delivery"`
	StockLevel  int `yaml:"stock_level"`
}

type TxTiming struct {
	NewOrder    int `yaml:"new_order"`
	Payment     int `yaml:"payment"`
	OrderStatus int `yaml:"order_status"`
	Delivery    int `yaml:"delivery"`
	StockLevel  int `yaml:"stock_level"`
}

type NamedHost struct {
	Name string `yaml:"name"`
	Host string `yaml:"host"`
}

type Phases struct {
	StartLead        string `yaml:"start_lead"`
	RampUp           string `yaml:"ramp_up"`
	Measurement      string `yaml:"measurement"`
	TransactionDrain string `yaml:"transaction_drain"`
	AsyncWorkDrain   string `yaml:"async_work_drain"`
	StopGrace        string `yaml:"stop_grace"`
	MaxClockSkew     string `yaml:"max_clock_skew_ms"`
}

type Runtime struct {
	Pacing                string      `yaml:"pacing"`
	ThinkTimeDistribution string      `yaml:"think_time_distribution"`
	ThreadsPerWorker      int         `yaml:"threads_per_worker"`
	MaxInflightPerWorker  int         `yaml:"max_inflight_per_worker"`
	Retry                 RetryPolicy `yaml:"retry"`
	Histogram             Histogram   `yaml:"histogram"`
}

type RetryPolicy struct {
	MaxAttempts    int    `yaml:"max_attempts"`
	InitialBackoff string `yaml:"initial_backoff"`
	MaxBackoff     string `yaml:"max_backoff"`
	Jitter         string `yaml:"jitter"`
}

// Histogram configures the worker linear_exp latency histogram.
// Only unit and highest affect layout; buckets are [0, hdr_till) linear
// then exponential up to highest (hdr_till is an implementation default).
type Histogram struct {
	Unit    string `yaml:"unit"`
	Highest int64  `yaml:"highest"`
}

type Checks struct {
	AfterImport bool `yaml:"after_import"`
	AfterRun    bool `yaml:"after_run"`
	FailFast    bool `yaml:"fail_fast"`
}

type Collect struct {
	IncludeEvents bool `yaml:"include_events"`
	IncludeLogs   bool `yaml:"include_logs"`
}

// ParseFile reads and parses a profile YAML file.
func ParseFile(path string) (*Profile, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return Parse(data)
}

// Parse decodes profile YAML.
// Unknown fields are rejected (specification §10).
func Parse(data []byte) (*Profile, error) {
	var raw map[string]interface{}
	if err := yaml.Unmarshal(data, &raw); err != nil {
		return nil, fmt.Errorf("yaml decode: %w", err)
	}
	var p Profile
	dec := yaml.NewDecoder(bytes.NewReader(data))
	dec.KnownFields(true)
	if err := dec.Decode(&p); err != nil {
		return nil, fmt.Errorf("profile decode: %w", err)
	}
	p.Raw = raw
	return &p, nil
}

// ValidateInstanceName checks instance name pattern.
func ValidateInstanceName(name string) error {
	if !instanceNameRE.MatchString(name) {
		return fmt.Errorf("instance name %q must match [a-z][a-z0-9-]*", name)
	}
	return nil
}

// LoaderInstances returns assignment.Instance list for loaders.
func (p *Profile) LoaderInstances() []assignment.Instance {
	out := make([]assignment.Instance, len(p.Loaders))
	for i, l := range p.Loaders {
		out[i] = assignment.Instance{Name: l.Name, Host: l.Host}
	}
	return out
}

// WorkerInstances returns assignment.Instance list for workers.
func (p *Profile) WorkerInstances() []assignment.Instance {
	out := make([]assignment.Instance, len(p.Workers))
	for i, w := range p.Workers {
		out[i] = assignment.Instance{Name: w.Name, Host: w.Host}
	}
	return out
}

// ParseDurationMs parses profile duration strings like "45s", "5m", "100ms".
func ParseDurationMs(s string) (int64, error) {
	if s == "" {
		return 0, fmt.Errorf("empty duration")
	}
	if isDigits(s) {
		v, err := parseInt64(s)
		if err != nil {
			return 0, err
		}
		return v, nil
	}
	d, err := time.ParseDuration(s)
	if err != nil {
		return 0, fmt.Errorf("invalid duration %q: %w", s, err)
	}
	return d.Milliseconds(), nil
}

func isDigits(s string) bool {
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return len(s) > 0
}

func parseInt64(s string) (int64, error) {
	var v int64
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, fmt.Errorf("invalid integer %q", s)
		}
		v = v*10 + int64(c-'0')
	}
	return v, nil
}

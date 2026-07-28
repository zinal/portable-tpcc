package config

import "time"

const APIVersion = "tpcectl/v1"

// Profile is the declarative tpcectl configuration (spec-orchestrator §5).
type Profile struct {
	APIVersion string `yaml:"apiVersion"`
	Kind       string `yaml:"kind"`
	Name       string `yaml:"name"`
	RunID      string `yaml:"run_id"`

	SSH SSHConfig `yaml:"ssh"`

	Paths PathsConfig `yaml:"paths"`

	Scale ScaleConfig `yaml:"scale"`

	BaseTimeEpoch   *int64 `yaml:"base_time_epoch"`
	BaseTimeLeadSec int    `yaml:"base_time_lead_sec"`

	Timeouts TimeoutsConfig `yaml:"timeouts"`

	DB DBConfig `yaml:"db"`

	Schema SchemaConfig `yaml:"schema"`

	Hosts map[string]HostConfig `yaml:"hosts"`

	Deploy DeployConfig `yaml:"deploy"`

	Load LoadConfig `yaml:"load"`

	BH []BHInstance `yaml:"bh"`
	MEE []MEEInstance `yaml:"mee"`
	DM *DMInstance `yaml:"dm"`
	CE []CEInstance `yaml:"ce"`

	StandaloneDriver *StandaloneDriverConfig `yaml:"standalone_driver"`

	Collect CollectConfig `yaml:"collect"`
}

type SSHConfig struct {
	User           string        `yaml:"user"`
	Port           int           `yaml:"port"`
	PrivateKey     string        `yaml:"private_key"`
	KnownHosts     string        `yaml:"known_hosts"`
	ConnectTimeout time.Duration `yaml:"connect_timeout"`
	UseAgent       *bool         `yaml:"use_agent"`
}

type HostSSHConfig struct {
	User           string        `yaml:"user"`
	Port           int           `yaml:"port"`
	PrivateKey     string        `yaml:"private_key"`
	KnownHosts     string        `yaml:"known_hosts"`
	ConnectTimeout time.Duration `yaml:"connect_timeout"`
	UseAgent       *bool         `yaml:"use_agent"`
}

type HostConfig struct {
	Address string         `yaml:"address"`
	SSH     *HostSSHConfig `yaml:"ssh"`
}

type PathsConfig struct {
	LocalBin   string `yaml:"local_bin"`
	LocalData  string `yaml:"local_data"`
	LocalSQL   string `yaml:"local_sql"`
	RemoteRoot string `yaml:"remote_root"`
}

type ScaleConfig struct {
	Customers        int  `yaml:"customers"`
	ActiveCustomers  int  `yaml:"active_customers"`
	ScaleFactor      int  `yaml:"scale_factor"`
	InitialTradeDays int  `yaml:"initial_trade_days"`
	DurationSec      int  `yaml:"duration_sec"`
	ClientSide       bool `yaml:"client_side"`
}

type TimeoutsConfig struct {
	ConfigDistribute   time.Duration `yaml:"config_distribute"`
	Ready              time.Duration `yaml:"ready"`
	CleanupWait        time.Duration `yaml:"cleanup_wait"`
	CECompletionGrace  time.Duration `yaml:"ce_completion_grace"`
	MEEDrain           time.Duration `yaml:"mee_drain"`
	StopGrace          time.Duration `yaml:"stop_grace"`
}

type DBConfig struct {
	Host        string `yaml:"host"`
	Port        int    `yaml:"port"`
	Name        string `yaml:"name"`
	User        string `yaml:"user"`
	PasswordEnv string `yaml:"password_env"`
	SSLMode     string `yaml:"sslmode"`
}

type SchemaConfig struct {
	Mode         string `yaml:"mode"`
	Partitions   int    `yaml:"partitions"`
	ApplyIndexes bool   `yaml:"apply_indexes"`
	ApplyFKs     bool   `yaml:"apply_fks"`
}

type DeployArtifact struct {
	Src       string `yaml:"src"`
	Dst       string `yaml:"dst"`
	Mode      string `yaml:"mode"`
	Recursive bool   `yaml:"recursive"`
	Optional  bool   `yaml:"optional"`
}

type DeployConfig struct {
	Artifacts    []DeployArtifact `yaml:"artifacts"`
	UseTarUpload *bool            `yaml:"use_tar_upload"`
}

// TarUploadEnabled reports whether recursive deploy uses tar streaming (default true).
func (d DeployConfig) TarUploadEnabled() bool {
	if d.UseTarUpload == nil {
		return true
	}
	return *d.UseTarUpload
}

type LoadShard struct {
	Host  string `yaml:"host"`
	Begin int    `yaml:"begin"`
	Count int    `yaml:"count"`
}

type LoadConfig struct {
	Shards []LoadShard `yaml:"shards"`
}

type BHInstance struct {
	Name   string `yaml:"name"`
	Host   string `yaml:"host"`
	Listen int    `yaml:"listen"`
	Output string `yaml:"output"`
}

type MEEInstance struct {
	Name     string `yaml:"name"`
	Host     string `yaml:"host"`
	Listen   int    `yaml:"listen"`
	UniqueID int    `yaml:"unique_id"`
	Output   string `yaml:"output"`
}

type DMInstance struct {
	Name   string `yaml:"name"`
	Host   string `yaml:"host"`
	Output string `yaml:"output"`
}

type CEPartition struct {
	StartID int `yaml:"start_id"`
	Count   int `yaml:"count"`
	Percent int `yaml:"percent"`
}

type CEInstance struct {
	Name      string       `yaml:"name"`
	Host      string       `yaml:"host"`
	Users     int          `yaml:"users"`
	CEIDBase  int          `yaml:"ce_id_base"`
	Partition *CEPartition `yaml:"partition"`
	Output    string       `yaml:"output"`
}

type StandaloneDriverConfig struct {
	Enabled     bool   `yaml:"enabled"`
	Host        string `yaml:"host"`
	Users       int    `yaml:"users"`
	CEIDBase    int    `yaml:"ce_id_base"`
	DurationSec int    `yaml:"duration_sec"`
	Output      string `yaml:"output"`
}

type CollectConfig struct {
	Dest             string `yaml:"dest"`
	PostCommand      string `yaml:"post_command"`
	EGenTesterExport string `yaml:"egen_tester_export"`
}

// ResolvedProfile holds a profile after defaults, template expansion, and path resolution.
type ResolvedProfile struct {
	Profile
	ProfilePath      string
	AbsolutePaths    PathsConfig
	EffectiveRunID   string
	HostAddresses    map[string]string
}

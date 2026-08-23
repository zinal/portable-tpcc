package cli

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"

	"portable-tpcc/mind/internal/profile"
	"portable-tpcc/mind/internal/validate"
)

var errProfileExists = errors.New("profile already exists")

type configureOpts struct {
	ProfilePath string
	DBMS        string
	Yes         bool

	Name                  *string
	SSHUser               *string
	UseAgent              *bool
	KnownHosts            *string
	ConnectTimeout        *string
	InsecureIgnore        *bool
	LocalArtifacts        *string
	RemoteRoot            *string
	ResultRoot            *string
	StateDir              *string
	Endpoint              *string
	Database              *string
	Path                  *string
	User                  *string
	PasswordEnv           *string
	AuthScheme            *string
	SaKeyFile             *string
	CaFile                *string
	Warehouses            *int
	Seed                  *int64
	BatchRows             *int
	TerminalsPerWarehouse *int
	MixNewOrder           *int
	MixPayment            *int
	MixOrderStatus        *int
	MixDelivery           *int
	MixStockLevel         *int
	KeyingNewOrder        *int
	KeyingPayment         *int
	KeyingOrderStatus     *int
	KeyingDelivery        *int
	KeyingStockLevel      *int
	ThinkNewOrder         *int
	ThinkPayment          *int
	ThinkOrderStatus      *int
	ThinkDelivery         *int
	ThinkStockLevel       *int
	Loaders               []string
	Workers               []string
	StartLead             *string
	RampUp                *string
	Measurement           *string
	TransactionDrain      *string
	AsyncWorkDrain        *string
	StopGrace             *string
	MaxClockSkew          *string
	Pacing                *string
	ThinkTimeDistribution *string
	ThreadsPerLoader      *int
	ThreadsPerWorker      *int
	CheckConcurrency      *int
	MaxInflightPerWorker  *int
	RetryMaxAttempts      *int
	RetryInitialBackoff   *string
	RetryMaxBackoff       *string
	RetryJitter           *string
	HistogramUnit         *string
	HistogramHighest      *int64
	AfterImport           *bool
	AfterTest             *bool
	FailFast              *bool
	IncludeEvents         *bool
	IncludeLogs           *bool
	Partitioning          *string
	PartitionCount        *int
	ForeignKeys           *string
	Partitions            *int
	QueryTimeout          *int
	IndexParallel         *int
}

func runConfigure(args []string) int {
	opts, err := parseConfigureArgs(args)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	p, err := buildConfigureProfile(opts)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	res := validate.Profile(p)
	if !res.Valid {
		fmt.Fprintf(os.Stderr, "error: generated profile is invalid:\n")
		for _, e := range res.Errors {
			fmt.Fprintf(os.Stderr, "  %s\n", e)
		}
		return 1
	}
	data, err := profile.EncodeExample(p)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := writeProfileFile(opts.ProfilePath, data, opts.Yes); err != nil {
		fmt.Fprintln(os.Stderr, err)
		if errors.Is(err, errProfileExists) {
			return 2
		}
		return 1
	}
	fmt.Printf("wrote %s\n", opts.ProfilePath)
	return 0
}

func parseConfigureArgs(args []string) (*configureOpts, error) {
	opts := &configureOpts{}
	var positionals []string
	for i := 0; i < len(args); i++ {
		arg := args[i]
		switch {
		case arg == "--profile" || strings.HasPrefix(arg, "--profile="):
			val, next, err := requireFlagValue(args, i, "--profile")
			if err != nil {
				return nil, err
			}
			opts.ProfilePath = val
			i = next
		case arg == "--dbms" || strings.HasPrefix(arg, "--dbms="):
			val, next, err := requireFlagValue(args, i, "--dbms")
			if err != nil {
				return nil, err
			}
			opts.DBMS = val
			i = next
		case arg == "--yes":
			opts.Yes = true
		case arg == "--name" || strings.HasPrefix(arg, "--name="):
			val, next, err := requireFlagValue(args, i, "--name")
			if err != nil {
				return nil, err
			}
			opts.Name = &val
			i = next
		case arg == "--ssh-user" || strings.HasPrefix(arg, "--ssh-user="):
			val, next, err := requireFlagValue(args, i, "--ssh-user")
			if err != nil {
				return nil, err
			}
			opts.SSHUser = &val
			i = next
		case arg == "--use-agent" || strings.HasPrefix(arg, "--use-agent="):
			v, next, err := requireFlagBool(args, i, "--use-agent")
			if err != nil {
				return nil, err
			}
			opts.UseAgent = &v
			i = next
		case arg == "--known-hosts" || strings.HasPrefix(arg, "--known-hosts="):
			val, next, err := requireFlagValue(args, i, "--known-hosts")
			if err != nil {
				return nil, err
			}
			opts.KnownHosts = &val
			i = next
		case arg == "--connect-timeout" || strings.HasPrefix(arg, "--connect-timeout="):
			val, next, err := requireFlagValue(args, i, "--connect-timeout")
			if err != nil {
				return nil, err
			}
			opts.ConnectTimeout = &val
			i = next
		case arg == "--insecure-ignore-host-key" || strings.HasPrefix(arg, "--insecure-ignore-host-key="):
			v, next, err := requireFlagBool(args, i, "--insecure-ignore-host-key")
			if err != nil {
				return nil, err
			}
			opts.InsecureIgnore = &v
			i = next
		case arg == "--local-artifacts" || strings.HasPrefix(arg, "--local-artifacts="):
			val, next, err := requireFlagValue(args, i, "--local-artifacts")
			if err != nil {
				return nil, err
			}
			opts.LocalArtifacts = &val
			i = next
		case arg == "--remote-root" || strings.HasPrefix(arg, "--remote-root="):
			val, next, err := requireFlagValue(args, i, "--remote-root")
			if err != nil {
				return nil, err
			}
			opts.RemoteRoot = &val
			i = next
		case arg == "--result-root" || strings.HasPrefix(arg, "--result-root="):
			val, next, err := requireFlagValue(args, i, "--result-root")
			if err != nil {
				return nil, err
			}
			opts.ResultRoot = &val
			i = next
		case arg == "--state-dir" || strings.HasPrefix(arg, "--state-dir="):
			val, next, err := requireFlagValue(args, i, "--state-dir")
			if err != nil {
				return nil, err
			}
			opts.StateDir = &val
			i = next
		case arg == "--endpoint" || strings.HasPrefix(arg, "--endpoint="):
			val, next, err := requireFlagValue(args, i, "--endpoint")
			if err != nil {
				return nil, err
			}
			opts.Endpoint = &val
			i = next
		case arg == "--database" || strings.HasPrefix(arg, "--database="):
			val, next, err := requireFlagValue(args, i, "--database")
			if err != nil {
				return nil, err
			}
			opts.Database = &val
			i = next
		case arg == "--path" || strings.HasPrefix(arg, "--path="):
			val, next, err := requireFlagValue(args, i, "--path")
			if err != nil {
				return nil, err
			}
			opts.Path = &val
			i = next
		case arg == "--user" || strings.HasPrefix(arg, "--user="):
			val, next, err := requireFlagValue(args, i, "--user")
			if err != nil {
				return nil, err
			}
			opts.User = &val
			i = next
		case arg == "--password-env" || strings.HasPrefix(arg, "--password-env="):
			val, next, err := requireFlagValue(args, i, "--password-env")
			if err != nil {
				return nil, err
			}
			opts.PasswordEnv = &val
			i = next
		case arg == "--auth-scheme" || strings.HasPrefix(arg, "--auth-scheme="):
			val, next, err := requireFlagValue(args, i, "--auth-scheme")
			if err != nil {
				return nil, err
			}
			opts.AuthScheme = &val
			i = next
		case arg == "--sa-key-file" || strings.HasPrefix(arg, "--sa-key-file="):
			val, next, err := requireFlagValue(args, i, "--sa-key-file")
			if err != nil {
				return nil, err
			}
			opts.SaKeyFile = &val
			i = next
		case arg == "--ca-file" || strings.HasPrefix(arg, "--ca-file="):
			val, next, err := requireFlagValue(args, i, "--ca-file")
			if err != nil {
				return nil, err
			}
			opts.CaFile = &val
			i = next
		case arg == "--warehouses" || strings.HasPrefix(arg, "--warehouses="):
			n, next, err := requireFlagInt(args, i, "--warehouses")
			if err != nil {
				return nil, err
			}
			opts.Warehouses = &n
			i = next
		case arg == "--seed" || strings.HasPrefix(arg, "--seed="):
			n, next, err := requireFlagInt64(args, i, "--seed")
			if err != nil {
				return nil, err
			}
			opts.Seed = &n
			i = next
		case arg == "--batch-rows" || strings.HasPrefix(arg, "--batch-rows="):
			n, next, err := requireFlagInt(args, i, "--batch-rows")
			if err != nil {
				return nil, err
			}
			opts.BatchRows = &n
			i = next
		case arg == "--terminals-per-warehouse" || strings.HasPrefix(arg, "--terminals-per-warehouse="):
			n, next, err := requireFlagInt(args, i, "--terminals-per-warehouse")
			if err != nil {
				return nil, err
			}
			opts.TerminalsPerWarehouse = &n
			i = next
		case arg == "--mix-new-order" || strings.HasPrefix(arg, "--mix-new-order="):
			n, next, err := requireFlagInt(args, i, "--mix-new-order")
			if err != nil {
				return nil, err
			}
			opts.MixNewOrder = &n
			i = next
		case arg == "--mix-payment" || strings.HasPrefix(arg, "--mix-payment="):
			n, next, err := requireFlagInt(args, i, "--mix-payment")
			if err != nil {
				return nil, err
			}
			opts.MixPayment = &n
			i = next
		case arg == "--mix-order-status" || strings.HasPrefix(arg, "--mix-order-status="):
			n, next, err := requireFlagInt(args, i, "--mix-order-status")
			if err != nil {
				return nil, err
			}
			opts.MixOrderStatus = &n
			i = next
		case arg == "--mix-delivery" || strings.HasPrefix(arg, "--mix-delivery="):
			n, next, err := requireFlagInt(args, i, "--mix-delivery")
			if err != nil {
				return nil, err
			}
			opts.MixDelivery = &n
			i = next
		case arg == "--mix-stock-level" || strings.HasPrefix(arg, "--mix-stock-level="):
			n, next, err := requireFlagInt(args, i, "--mix-stock-level")
			if err != nil {
				return nil, err
			}
			opts.MixStockLevel = &n
			i = next
		case arg == "--keying-new-order" || strings.HasPrefix(arg, "--keying-new-order="):
			n, next, err := requireFlagInt(args, i, "--keying-new-order")
			if err != nil {
				return nil, err
			}
			opts.KeyingNewOrder = &n
			i = next
		case arg == "--keying-payment" || strings.HasPrefix(arg, "--keying-payment="):
			n, next, err := requireFlagInt(args, i, "--keying-payment")
			if err != nil {
				return nil, err
			}
			opts.KeyingPayment = &n
			i = next
		case arg == "--keying-order-status" || strings.HasPrefix(arg, "--keying-order-status="):
			n, next, err := requireFlagInt(args, i, "--keying-order-status")
			if err != nil {
				return nil, err
			}
			opts.KeyingOrderStatus = &n
			i = next
		case arg == "--keying-delivery" || strings.HasPrefix(arg, "--keying-delivery="):
			n, next, err := requireFlagInt(args, i, "--keying-delivery")
			if err != nil {
				return nil, err
			}
			opts.KeyingDelivery = &n
			i = next
		case arg == "--keying-stock-level" || strings.HasPrefix(arg, "--keying-stock-level="):
			n, next, err := requireFlagInt(args, i, "--keying-stock-level")
			if err != nil {
				return nil, err
			}
			opts.KeyingStockLevel = &n
			i = next
		case arg == "--think-new-order" || strings.HasPrefix(arg, "--think-new-order="):
			n, next, err := requireFlagInt(args, i, "--think-new-order")
			if err != nil {
				return nil, err
			}
			opts.ThinkNewOrder = &n
			i = next
		case arg == "--think-payment" || strings.HasPrefix(arg, "--think-payment="):
			n, next, err := requireFlagInt(args, i, "--think-payment")
			if err != nil {
				return nil, err
			}
			opts.ThinkPayment = &n
			i = next
		case arg == "--think-order-status" || strings.HasPrefix(arg, "--think-order-status="):
			n, next, err := requireFlagInt(args, i, "--think-order-status")
			if err != nil {
				return nil, err
			}
			opts.ThinkOrderStatus = &n
			i = next
		case arg == "--think-delivery" || strings.HasPrefix(arg, "--think-delivery="):
			n, next, err := requireFlagInt(args, i, "--think-delivery")
			if err != nil {
				return nil, err
			}
			opts.ThinkDelivery = &n
			i = next
		case arg == "--think-stock-level" || strings.HasPrefix(arg, "--think-stock-level="):
			n, next, err := requireFlagInt(args, i, "--think-stock-level")
			if err != nil {
				return nil, err
			}
			opts.ThinkStockLevel = &n
			i = next
		case arg == "--loaders" || strings.HasPrefix(arg, "--loaders="):
			val, next, err := requireFlagValue(args, i, "--loaders")
			if err != nil {
				return nil, err
			}
			hosts, err := splitHostList(val)
			if err != nil {
				return nil, fmt.Errorf("--loaders: %w", err)
			}
			opts.Loaders = append(opts.Loaders, hosts...)
			i = next
		case arg == "--workers" || strings.HasPrefix(arg, "--workers="):
			val, next, err := requireFlagValue(args, i, "--workers")
			if err != nil {
				return nil, err
			}
			hosts, err := splitHostList(val)
			if err != nil {
				return nil, fmt.Errorf("--workers: %w", err)
			}
			opts.Workers = append(opts.Workers, hosts...)
			i = next
		case arg == "--start-lead" || strings.HasPrefix(arg, "--start-lead="):
			val, next, err := requireFlagValue(args, i, "--start-lead")
			if err != nil {
				return nil, err
			}
			opts.StartLead = &val
			i = next
		case arg == "--ramp-up" || strings.HasPrefix(arg, "--ramp-up="):
			val, next, err := requireFlagValue(args, i, "--ramp-up")
			if err != nil {
				return nil, err
			}
			opts.RampUp = &val
			i = next
		case arg == "--measurement" || strings.HasPrefix(arg, "--measurement="):
			val, next, err := requireFlagValue(args, i, "--measurement")
			if err != nil {
				return nil, err
			}
			opts.Measurement = &val
			i = next
		case arg == "--transaction-drain" || strings.HasPrefix(arg, "--transaction-drain="):
			val, next, err := requireFlagValue(args, i, "--transaction-drain")
			if err != nil {
				return nil, err
			}
			opts.TransactionDrain = &val
			i = next
		case arg == "--async-work-drain" || strings.HasPrefix(arg, "--async-work-drain="):
			val, next, err := requireFlagValue(args, i, "--async-work-drain")
			if err != nil {
				return nil, err
			}
			opts.AsyncWorkDrain = &val
			i = next
		case arg == "--stop-grace" || strings.HasPrefix(arg, "--stop-grace="):
			val, next, err := requireFlagValue(args, i, "--stop-grace")
			if err != nil {
				return nil, err
			}
			opts.StopGrace = &val
			i = next
		case arg == "--max-clock-skew" || strings.HasPrefix(arg, "--max-clock-skew="):
			val, next, err := requireFlagValue(args, i, "--max-clock-skew")
			if err != nil {
				return nil, err
			}
			opts.MaxClockSkew = &val
			i = next
		case arg == "--pacing" || strings.HasPrefix(arg, "--pacing="):
			val, next, err := requireFlagValue(args, i, "--pacing")
			if err != nil {
				return nil, err
			}
			opts.Pacing = &val
			i = next
		case arg == "--think-time-distribution" || strings.HasPrefix(arg, "--think-time-distribution="):
			val, next, err := requireFlagValue(args, i, "--think-time-distribution")
			if err != nil {
				return nil, err
			}
			opts.ThinkTimeDistribution = &val
			i = next
		case arg == "--threads-per-loader" || strings.HasPrefix(arg, "--threads-per-loader="):
			n, next, err := requireFlagNonNegativeInt(args, i, "--threads-per-loader")
			if err != nil {
				return nil, err
			}
			opts.ThreadsPerLoader = &n
			i = next
		case arg == "--threads-per-worker" || strings.HasPrefix(arg, "--threads-per-worker="):
			n, next, err := requireFlagNonNegativeInt(args, i, "--threads-per-worker")
			if err != nil {
				return nil, err
			}
			opts.ThreadsPerWorker = &n
			i = next
		case arg == "--check-concurrency" || strings.HasPrefix(arg, "--check-concurrency="):
			n, next, err := requireFlagNonNegativeInt(args, i, "--check-concurrency")
			if err != nil {
				return nil, err
			}
			opts.CheckConcurrency = &n
			i = next
		case arg == "--max-inflight-per-worker" || strings.HasPrefix(arg, "--max-inflight-per-worker="):
			n, next, err := requireFlagNonNegativeInt(args, i, "--max-inflight-per-worker")
			if err != nil {
				return nil, err
			}
			opts.MaxInflightPerWorker = &n
			i = next
		case arg == "--retry-max-attempts" || strings.HasPrefix(arg, "--retry-max-attempts="):
			n, next, err := requireFlagNonNegativeInt(args, i, "--retry-max-attempts")
			if err != nil {
				return nil, err
			}
			opts.RetryMaxAttempts = &n
			i = next
		case arg == "--retry-initial-backoff" || strings.HasPrefix(arg, "--retry-initial-backoff="):
			val, next, err := requireFlagValue(args, i, "--retry-initial-backoff")
			if err != nil {
				return nil, err
			}
			opts.RetryInitialBackoff = &val
			i = next
		case arg == "--retry-max-backoff" || strings.HasPrefix(arg, "--retry-max-backoff="):
			val, next, err := requireFlagValue(args, i, "--retry-max-backoff")
			if err != nil {
				return nil, err
			}
			opts.RetryMaxBackoff = &val
			i = next
		case arg == "--retry-jitter" || strings.HasPrefix(arg, "--retry-jitter="):
			val, next, err := requireFlagValue(args, i, "--retry-jitter")
			if err != nil {
				return nil, err
			}
			opts.RetryJitter = &val
			i = next
		case arg == "--histogram-unit" || strings.HasPrefix(arg, "--histogram-unit="):
			val, next, err := requireFlagValue(args, i, "--histogram-unit")
			if err != nil {
				return nil, err
			}
			opts.HistogramUnit = &val
			i = next
		case arg == "--histogram-highest" || strings.HasPrefix(arg, "--histogram-highest="):
			n, next, err := requireFlagInt64(args, i, "--histogram-highest")
			if err != nil {
				return nil, err
			}
			opts.HistogramHighest = &n
			i = next
		case arg == "--after-import" || strings.HasPrefix(arg, "--after-import="):
			v, next, err := requireFlagBool(args, i, "--after-import")
			if err != nil {
				return nil, err
			}
			opts.AfterImport = &v
			i = next
		case arg == "--after-test" || strings.HasPrefix(arg, "--after-test="):
			v, next, err := requireFlagBool(args, i, "--after-test")
			if err != nil {
				return nil, err
			}
			opts.AfterTest = &v
			i = next
		case arg == "--fail-fast" || strings.HasPrefix(arg, "--fail-fast="):
			v, next, err := requireFlagBool(args, i, "--fail-fast")
			if err != nil {
				return nil, err
			}
			opts.FailFast = &v
			i = next
		case arg == "--include-events" || strings.HasPrefix(arg, "--include-events="):
			v, next, err := requireFlagBool(args, i, "--include-events")
			if err != nil {
				return nil, err
			}
			opts.IncludeEvents = &v
			i = next
		case arg == "--include-logs" || strings.HasPrefix(arg, "--include-logs="):
			v, next, err := requireFlagBool(args, i, "--include-logs")
			if err != nil {
				return nil, err
			}
			opts.IncludeLogs = &v
			i = next
		case arg == "--partitioning" || strings.HasPrefix(arg, "--partitioning="):
			val, next, err := requireFlagValue(args, i, "--partitioning")
			if err != nil {
				return nil, err
			}
			opts.Partitioning = &val
			i = next
		case arg == "--partition-count" || strings.HasPrefix(arg, "--partition-count="):
			n, next, err := requireFlagInt(args, i, "--partition-count")
			if err != nil {
				return nil, err
			}
			opts.PartitionCount = &n
			i = next
		case arg == "--foreign-keys" || strings.HasPrefix(arg, "--foreign-keys="):
			val, next, err := requireFlagValue(args, i, "--foreign-keys")
			if err != nil {
				return nil, err
			}
			opts.ForeignKeys = &val
			i = next
		case arg == "--partitions" || strings.HasPrefix(arg, "--partitions="):
			n, next, err := requireFlagInt(args, i, "--partitions")
			if err != nil {
				return nil, err
			}
			opts.Partitions = &n
			i = next
		case arg == "--query-timeout" || strings.HasPrefix(arg, "--query-timeout="):
			n, next, err := requireFlagInt(args, i, "--query-timeout")
			if err != nil {
				return nil, err
			}
			opts.QueryTimeout = &n
			i = next
		case arg == "--index-parallel" || strings.HasPrefix(arg, "--index-parallel="):
			n, next, err := requireFlagInt(args, i, "--index-parallel")
			if err != nil {
				return nil, err
			}
			opts.IndexParallel = &n
			i = next
		default:
			if strings.HasPrefix(arg, "-") {
				return nil, fmt.Errorf("error: unknown flag %s", arg)
			}
			positionals = append(positionals, arg)
		}
	}
	if opts.ProfilePath == "" {
		if len(positionals) == 0 {
			return nil, fmt.Errorf("error: configure requires --profile <path> (or a positional profile path)")
		}
		opts.ProfilePath = positionals[0]
		positionals = positionals[1:]
	}
	if len(positionals) > 0 {
		return nil, fmt.Errorf("error: unexpected argument %q", positionals[0])
	}
	if opts.DBMS == "" {
		return nil, fmt.Errorf("error: --dbms is required (pgsql, ydb, or oceanbase)")
	}
	if !profile.AllowedDBMS[opts.DBMS] {
		return nil, fmt.Errorf("error: unknown --dbms %q (want pgsql, ydb, or oceanbase)", opts.DBMS)
	}
	if err := rejectForeignDBMSFlags(opts); err != nil {
		return nil, err
	}
	return opts, nil
}

func rejectForeignDBMSFlags(opts *configureOpts) error {
	ydbOnly := []struct {
		set  bool
		name string
	}{
		{opts.AuthScheme != nil, "--auth-scheme"},
		{opts.SaKeyFile != nil, "--sa-key-file"},
		{opts.CaFile != nil, "--ca-file"},
	}
	for _, f := range ydbOnly {
		if f.set && opts.DBMS != "ydb" {
			return fmt.Errorf("error: %s is only valid for --dbms ydb", f.name)
		}
	}
	pgsqlOnly := []struct {
		set  bool
		name string
	}{
		{opts.Partitioning != nil, "--partitioning"},
		{opts.PartitionCount != nil, "--partition-count"},
	}
	for _, f := range pgsqlOnly {
		if f.set && opts.DBMS != "pgsql" {
			return fmt.Errorf("error: %s is only valid for --dbms pgsql", f.name)
		}
	}
	obOnly := []struct {
		set  bool
		name string
	}{
		{opts.Partitions != nil, "--partitions"},
		{opts.QueryTimeout != nil, "--query-timeout"},
		{opts.IndexParallel != nil, "--index-parallel"},
	}
	for _, f := range obOnly {
		if f.set && opts.DBMS != "oceanbase" {
			return fmt.Errorf("error: %s is only valid for --dbms oceanbase", f.name)
		}
	}
	if opts.ForeignKeys != nil && opts.DBMS != "pgsql" && opts.DBMS != "oceanbase" {
		return fmt.Errorf("error: --foreign-keys is only valid for --dbms pgsql or oceanbase")
	}
	return nil
}

func buildConfigureProfile(opts *configureOpts) (*profile.Profile, error) {
	name := profile.NameFromProfilePath(opts.ProfilePath)
	if opts.Name != nil {
		name = *opts.Name
	}
	p, err := profile.ExampleWithName(opts.DBMS, name, profile.DefaultSSHUser())
	if err != nil {
		return nil, err
	}
	applyString(&p.SSH.User, opts.SSHUser)
	applyBool(&p.SSH.UseAgent, opts.UseAgent)
	applyString(&p.SSH.KnownHosts, opts.KnownHosts)
	applyString(&p.SSH.ConnectTimeout, opts.ConnectTimeout)
	applyBool(&p.SSH.InsecureIgnore, opts.InsecureIgnore)
	applyString(&p.Paths.LocalArtifacts, opts.LocalArtifacts)
	applyString(&p.Paths.RemoteRoot, opts.RemoteRoot)
	applyString(&p.Paths.ResultRoot, opts.ResultRoot)
	applyString(&p.Paths.StateDir, opts.StateDir)
	applyString(&p.Database.Endpoint, opts.Endpoint)
	applyString(&p.Database.Database, opts.Database)
	applyString(&p.Database.Path, opts.Path)
	applyString(&p.Database.User, opts.User)
	applyString(&p.Database.PasswordEnv, opts.PasswordEnv)
	applyString(&p.Database.AuthScheme, opts.AuthScheme)
	applyString(&p.Database.SaKeyFile, opts.SaKeyFile)
	applyString(&p.Database.CaFile, opts.CaFile)
	if opts.Warehouses != nil {
		p.Scale.Warehouses = *opts.Warehouses
	}
	if opts.Seed != nil {
		seed := *opts.Seed
		p.Data.Seed = &seed
	}
	if opts.BatchRows != nil {
		p.Data.BatchRows = *opts.BatchRows
	}
	if opts.TerminalsPerWarehouse != nil {
		p.Workload.TerminalsPerWarehouse = *opts.TerminalsPerWarehouse
	}
	applyInt(&p.Workload.TransactionMix.NewOrder, opts.MixNewOrder)
	applyInt(&p.Workload.TransactionMix.Payment, opts.MixPayment)
	applyInt(&p.Workload.TransactionMix.OrderStatus, opts.MixOrderStatus)
	applyInt(&p.Workload.TransactionMix.Delivery, opts.MixDelivery)
	applyInt(&p.Workload.TransactionMix.StockLevel, opts.MixStockLevel)
	applyInt(&p.Workload.KeyingTimeMs.NewOrder, opts.KeyingNewOrder)
	applyInt(&p.Workload.KeyingTimeMs.Payment, opts.KeyingPayment)
	applyInt(&p.Workload.KeyingTimeMs.OrderStatus, opts.KeyingOrderStatus)
	applyInt(&p.Workload.KeyingTimeMs.Delivery, opts.KeyingDelivery)
	applyInt(&p.Workload.KeyingTimeMs.StockLevel, opts.KeyingStockLevel)
	applyInt(&p.Workload.ThinkTimeMs.NewOrder, opts.ThinkNewOrder)
	applyInt(&p.Workload.ThinkTimeMs.Payment, opts.ThinkPayment)
	applyInt(&p.Workload.ThinkTimeMs.OrderStatus, opts.ThinkOrderStatus)
	applyInt(&p.Workload.ThinkTimeMs.Delivery, opts.ThinkDelivery)
	applyInt(&p.Workload.ThinkTimeMs.StockLevel, opts.ThinkStockLevel)
	if len(opts.Loaders) > 0 {
		p.Loaders = profile.HostsFromStrings(opts.Loaders)
	}
	if len(opts.Workers) > 0 {
		p.Workers = profile.HostsFromStrings(opts.Workers)
	}
	applyString(&p.Phases.StartLead, opts.StartLead)
	applyString(&p.Phases.RampUp, opts.RampUp)
	applyString(&p.Phases.Measurement, opts.Measurement)
	applyString(&p.Phases.TransactionDrain, opts.TransactionDrain)
	applyString(&p.Phases.AsyncWorkDrain, opts.AsyncWorkDrain)
	applyString(&p.Phases.StopGrace, opts.StopGrace)
	applyString(&p.Phases.MaxClockSkew, opts.MaxClockSkew)
	applyString(&p.Runtime.Pacing, opts.Pacing)
	applyString(&p.Runtime.ThinkTimeDistribution, opts.ThinkTimeDistribution)
	applyInt(&p.Runtime.ThreadsPerLoader, opts.ThreadsPerLoader)
	applyInt(&p.Runtime.ThreadsPerWorker, opts.ThreadsPerWorker)
	applyInt(&p.Runtime.CheckConcurrency, opts.CheckConcurrency)
	applyInt(&p.Runtime.MaxInflightPerWorker, opts.MaxInflightPerWorker)
	applyInt(&p.Runtime.Retry.MaxAttempts, opts.RetryMaxAttempts)
	applyString(&p.Runtime.Retry.InitialBackoff, opts.RetryInitialBackoff)
	applyString(&p.Runtime.Retry.MaxBackoff, opts.RetryMaxBackoff)
	applyString(&p.Runtime.Retry.Jitter, opts.RetryJitter)
	applyString(&p.Runtime.Histogram.Unit, opts.HistogramUnit)
	if opts.HistogramHighest != nil {
		h := *opts.HistogramHighest
		p.Runtime.Histogram.Highest = &h
	}
	applyBool(&p.Checks.AfterImport, opts.AfterImport)
	applyBool(&p.Checks.AfterTest, opts.AfterTest)
	applyBool(&p.Checks.FailFast, opts.FailFast)
	applyBool(&p.Collect.IncludeEvents, opts.IncludeEvents)
	applyBool(&p.Collect.IncludeLogs, opts.IncludeLogs)
	if err := applyDatabaseOptions(p, opts); err != nil {
		return nil, err
	}
	reconcileYdbAuth(p, opts)
	return p, nil
}

func applyDatabaseOptions(p *profile.Profile, opts *configureOpts) error {
	if p.Database.Options == nil && (opts.Partitioning != nil || opts.PartitionCount != nil ||
		opts.ForeignKeys != nil || opts.Partitions != nil || opts.QueryTimeout != nil || opts.IndexParallel != nil) {
		p.Database.Options = map[string]interface{}{}
	}
	if opts.Partitioning != nil {
		p.Database.Options["partitioning"] = *opts.Partitioning
	}
	if opts.PartitionCount != nil {
		if partitioning, _ := p.Database.Options["partitioning"].(string); partitioning == "" || partitioning == "none" {
			p.Database.Options["partitioning"] = "warehouse_hash"
		}
		p.Database.Options["partition_count"] = *opts.PartitionCount
	}
	if opts.ForeignKeys != nil {
		p.Database.Options["foreign_keys"] = *opts.ForeignKeys
	}
	if opts.Partitions != nil {
		p.Database.Options["partitions"] = *opts.Partitions
	}
	if opts.QueryTimeout != nil {
		p.Database.Options["query_timeout"] = *opts.QueryTimeout
	}
	if opts.IndexParallel != nil {
		p.Database.Options["index_parallel"] = *opts.IndexParallel
	}
	return nil
}

func reconcileYdbAuth(p *profile.Profile, opts *configureOpts) {
	if p.Database.DBMS != "ydb" {
		return
	}
	explicit := opts.AuthScheme != nil
	if explicit {
		switch p.Database.AuthScheme {
		case "login":
			if p.Database.User == "" {
				p.Database.User = profile.DefaultYDBUser
			}
			if p.Database.PasswordEnv == "" {
				p.Database.PasswordEnv = profile.DefaultYDBPasswordEnv
			}
			p.Database.SaKeyFile = ""
		case "sa_key":
			p.Database.User = ""
			p.Database.PasswordEnv = ""
		case "anonymous":
			p.Database.User = ""
			p.Database.PasswordEnv = ""
			p.Database.SaKeyFile = ""
		}
		return
	}
	if opts.SaKeyFile != nil {
		p.Database.AuthScheme = "sa_key"
		p.Database.User = ""
		p.Database.PasswordEnv = ""
		return
	}
	if opts.User != nil || opts.PasswordEnv != nil {
		p.Database.AuthScheme = "login"
		if p.Database.User == "" {
			p.Database.User = profile.DefaultYDBUser
		}
		if p.Database.PasswordEnv == "" {
			p.Database.PasswordEnv = profile.DefaultYDBPasswordEnv
		}
		p.Database.SaKeyFile = ""
	}
}

func writeProfileFile(path string, data []byte, overwrite bool) error {
	if path == "" {
		return fmt.Errorf("error: profile path is empty")
	}
	if _, err := os.Stat(path); err == nil {
		if !overwrite {
			return fmt.Errorf("error: %s already exists (pass --yes to overwrite): %w", path, errProfileExists)
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	return os.WriteFile(path, data, 0644)
}

func splitHostList(val string) ([]string, error) {
	parts := strings.Split(val, ",")
	var out []string
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		out = append(out, p)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("host list must not be empty")
	}
	return out, nil
}

func applyString(dst *string, src *string) {
	if src != nil {
		*dst = *src
	}
}

func applyBool(dst *bool, src *bool) {
	if src != nil {
		*dst = *src
	}
}

func applyInt(dst *int, src *int) {
	if src != nil {
		*dst = *src
	}
}

func requireFlagBool(rest []string, i int, name string) (bool, int, error) {
	arg := rest[i]
	if prefix := name + "="; strings.HasPrefix(arg, prefix) {
		raw := arg[len(prefix):]
		switch raw {
		case "true", "1", "yes":
			return true, i, nil
		case "false", "0", "no":
			return false, i, nil
		default:
			return false, i, fmt.Errorf("%s: invalid boolean %q", name, raw)
		}
	}
	return true, i, nil
}

func requireFlagInt64(rest []string, i int, name string) (int64, int, error) {
	raw, next, err := requireFlagValue(rest, i, name)
	if err != nil {
		return 0, i, err
	}
	n, err := strconv.ParseInt(raw, 10, 64)
	if err != nil {
		return 0, i, fmt.Errorf("%s: invalid integer %q", name, raw)
	}
	return n, next, nil
}

func printConfigureUsage() {
	usage := strings.TrimSpace(`
mind-tpcc configure — write a complete example profile YAML

Usage:
  mind-tpcc configure --profile <path> --dbms <pgsql|ydb|oceanbase> [options]
  mind-tpcc configure <path> --dbms <pgsql|ydb|oceanbase> [options]

Required:
  --profile <path>             Output profile YAML (also accepted as a positional path)
  --dbms <pgsql|ydb|oceanbase> Database type

Omitted settings use built-in defaults. Host lists default to localhost.
The file contains every profile field, including DBMS-specific database keys.

Common overrides:
  --name <name>                metadata.name (default: sanitized filename)
  --ssh-user <user>            ssh.user (default: current account)
  --use-agent[=true|false]     ssh.use_agent
  --known-hosts <path>         ssh.known_hosts
  --connect-timeout <duration> ssh.connect_timeout
  --insecure-ignore-host-key   ssh.insecure_ignore_host_key
  --local-artifacts <path>     paths.local_artifacts
  --remote-root <path>         paths.remote_root
  --result-root <path>         paths.result_root
  --state-dir <path>           paths.state_dir
  --endpoint <addr>            database.endpoint
  --database <name>            database.database
  --path <path>                database.path
  --user <user>                database.user
  --password-env <name>        database.password_env
  --warehouses <n>             scale.warehouses
  --seed <n>                   data.seed
  --batch-rows <n>             data.batch_rows
  --loaders <hosts>            comma-separated or repeatable loader hosts
  --workers <hosts>            comma-separated or repeatable worker hosts
  --start-lead <duration>      phases.start_lead
  --ramp-up <duration>         phases.ramp_up
  --measurement <duration>     phases.measurement
  --transaction-drain <dur>    phases.transaction_drain
  --async-work-drain <dur>     phases.async_work_drain
  --stop-grace <duration>      phases.stop_grace
  --max-clock-skew <duration>  phases.max_clock_skew_ms
  --pacing <enabled|disabled>  runtime.pacing
  --think-time-distribution <name>
  --threads-per-loader <n>
  --threads-per-worker <n>
  --check-concurrency <n>
  --max-inflight-per-worker <n>
  --retry-max-attempts <n>
  --retry-initial-backoff <dur>
  --retry-max-backoff <dur>
  --retry-jitter <full|none>
  --histogram-unit <ms|us>
  --histogram-highest <n>
  --after-import[=true|false]  checks.after_import
  --after-test[=true|false]    checks.after_test
  --fail-fast[=true|false]     checks.fail_fast
  --include-events[=true|false]
  --include-logs[=true|false]
  --yes                        overwrite an existing file

Workload overrides:
  --terminals-per-warehouse <n>
  --mix-new-order <n>          --mix-payment <n> --mix-order-status <n>
  --mix-delivery <n>           --mix-stock-level <n>
  --keying-new-order <n>       --keying-payment <n> --keying-order-status <n>
  --keying-delivery <n>        --keying-stock-level <n>
  --think-new-order <n>        --think-payment <n> --think-order-status <n>
  --think-delivery <n>         --think-stock-level <n>

YDB:
  --auth-scheme <anonymous|login|sa_key>
  --sa-key-file <path>         --ca-file <path>

PostgreSQL:
  --partitioning <none|warehouse_hash>
  --partition-count <n>        --foreign-keys <on|off>

OceanBase:
  --partitions <n>             --foreign-keys <on|off>
  --query-timeout <seconds>    --index-parallel <n>
`)
	fmt.Println(usage)
}

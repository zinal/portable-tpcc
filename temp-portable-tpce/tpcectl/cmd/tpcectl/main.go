package main

import (
	"fmt"
	"os"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/cli"
)

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}

	var code int
	switch os.Args[1] {
	case "validate":
		code = cli.RunValidate(os.Args[2:])
	case "plan":
		code = cli.RunPlan(os.Args[2:])
	case "deploy":
		code = cli.RunDeploy(os.Args[2:])
	case "cleanup":
		code = cli.RunCleanup(os.Args[2:])
	case "schema":
		code = cli.RunSchema(os.Args[2:])
	case "load":
		code = cli.RunLoad(os.Args[2:])
	case "start":
		code = cli.RunStart(os.Args[2:])
	case "stop":
		code = cli.RunStop(os.Args[2:])
	case "status":
		code = cli.RunStatus(os.Args[2:])
	case "run":
		code = cli.RunRun(os.Args[2:])
	case "collect":
		code = cli.RunCollect(os.Args[2:])
	case "smoke":
		code = cli.RunSmoke(os.Args[2:])
	case "-h", "--help", "help":
		usage()
		code = 0
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n", os.Args[1])
		usage()
		code = 2
	}
	os.Exit(code)
}

func usage() {
	fmt.Fprintf(os.Stderr, `tpcectl - TPC-E testbed orchestrator

Usage:
  tpcectl <command> -f PROFILE.yaml [flags]

Commands (M1–M5):
  validate   Validate profile YAML
  plan       Print planned actions without side effects
  deploy     Copy artifacts to runtime hosts and write deployment manifest
  cleanup    Remove paths listed in deployment manifest (--yes)
  schema     Apply PostgreSQL DDL from paths.local_sql
  load       Run Loader shards in parallel
  run        Full lifecycle: optional deploy/schema/load, start, wait, stop, collect
  start      Start BH/MEE/DM/CE sequence and update run-state to running
  stop       Stop processes using normative CE→drain→DM→MEE→BH sequence
  status     Report PID aliveness and listen ports from run-state
  collect    Download instance outputs and orchestrator metadata
  smoke      Short run with --duration-sec and optional --users overrides

Global flags:
  -f, --file PROFILE.yaml
  -v, --verbose
  --dry-run
  --run-id ID
  --state-dir PATH

See docs/spec-orchestrator.md
`)
}

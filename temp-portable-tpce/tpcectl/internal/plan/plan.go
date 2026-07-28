package plan

import (
	"encoding/json"
	"fmt"
	"io"
	"sort"
	"strings"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/argv"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/runtimeconfig"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/schema"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/redact"
)

// For selects which operation to plan.
type For string

const (
	ForDeploy For = "deploy"
	ForSchema For = "schema"
	ForLoad   For = "load"
	ForRun    For = "run"
	ForStart  For = "start"
	ForStop   For = "stop"
)

// Options controls plan rendering.
type Options struct {
	For           For
	Now           time.Time
	BaseTimeEpoch *int64
	Verbose       bool
}

// Write renders a dry-run plan to w.
func Write(w io.Writer, r *config.ResolvedProfile, opts Options) error {
	if r == nil {
		return fmt.Errorf("profile is nil")
	}
	switch opts.For {
	case ForDeploy:
		return writeDeploy(w, r)
	case ForSchema:
		return writeSchema(w, r)
	case ForLoad:
		return writeLoad(w, r)
	case ForRun, ForStart:
		return writeRun(w, r, opts, opts.For == ForRun)
	case ForStop:
		return writeStop(w, r)
	case "":
		return fmt.Errorf("--for is required (deploy|schema|load|run|start|stop)")
	default:
		return fmt.Errorf("unsupported --for value %q", opts.For)
	}
}

func writeDeploy(w io.Writer, r *config.ResolvedProfile) error {
	fmt.Fprintf(w, "plan deploy profile=%s run_id=%s\n", r.Name, r.EffectiveRunID)
	hosts := r.DeployHosts()
	sort.Strings(hosts)
	for _, host := range hosts {
		fmt.Fprintf(w, "\n[%s]\n", host)
		fmt.Fprintf(w, "  mkdir -p %s\n", r.Paths.RemoteRoot)
		for _, art := range r.Deploy.Artifacts {
			fmt.Fprintf(w, "  copy %s -> %s/%s\n", art.Src, r.Paths.RemoteRoot, art.Dst)
		}
		fmt.Fprintf(w, "  write %s/.tpcectl/deploy-manifest.json\n", r.Paths.RemoteRoot)
	}
	return nil
}

func writeSchema(w io.Writer, r *config.ResolvedProfile) error {
	mode := r.Schema.Mode
	file := "create_tables.sql"
	if mode == "partitioned" {
		file = "create_tables_partitioned.sql"
	}
	fmt.Fprintf(w, "plan schema profile=%s\n", r.Name)
	fmt.Fprintf(w, "  apply %s/%s\n", r.Paths.LocalSQL, file)
	deferToLoad := schema.IndexesDeferred(r)
	if r.Schema.ApplyIndexes {
		if deferToLoad {
			fmt.Fprintf(w, "  defer indexes until after load\n")
		} else {
			fmt.Fprintf(w, "  apply indexes\n")
		}
	}
	if r.Schema.ApplyFKs {
		if deferToLoad {
			fmt.Fprintf(w, "  defer foreign keys until after load\n")
		} else {
			fmt.Fprintf(w, "  apply foreign keys\n")
		}
	}
	return nil
}

func writeLoad(w io.Writer, r *config.ResolvedProfile) error {
	fmt.Fprintf(w, "plan load profile=%s customers=%d\n", r.Name, r.Scale.Customers)
	for _, shard := range r.Load.Shards {
		fmt.Fprintf(w, "  [%s] Loader.exe -b %d -c %d\n", shard.Host, shard.Begin, shard.Count)
	}
	if schema.IndexesDeferred(r) {
		if r.Schema.ApplyIndexes {
			fmt.Fprintf(w, "  apply indexes after load\n")
		}
		if r.Schema.ApplyFKs {
			fmt.Fprintf(w, "  apply foreign keys after load\n")
		}
	}
	return nil
}

func writeRun(w io.Writer, r *config.ResolvedProfile, opts Options, waitCE bool) error {
	now := opts.Now
	if now.IsZero() {
		now = time.Now().UTC()
	}

	doc, raw, hash, err := runtimeconfig.Build(r, runtimeconfig.BuildOptions{
		Now:           now,
		BaseTimeEpoch: opts.BaseTimeEpoch,
	})
	if err != nil {
		return err
	}
	preview, _ := runtimeconfig.PreviewBaseTime(r, runtimeconfig.BuildOptions{
		Now:           now,
		BaseTimeEpoch: opts.BaseTimeEpoch,
	})

	cmd := "start"
	if waitCE {
		cmd = "run"
	}
	fmt.Fprintf(w, "plan %s profile=%s run_id=%s\n", cmd, r.Name, r.EffectiveRunID)
	fmt.Fprintf(w, "\n== run-config ==\n")
	fmt.Fprintf(w, "path: %s\n", r.RemoteRunConfigPath())
	fmt.Fprintf(w, "sha256: %s\n", hash)
	if preview.Explicit {
		fmt.Fprintf(w, "base_time_epoch: %d (explicit)\n", preview.Value)
	} else {
		fmt.Fprintf(w, "base_time_epoch: %d (preview; %s)\n", preview.Value, preview.Formula)
	}

	redacted := runtimeconfig.Redact(doc)
	enc, err := json.MarshalIndent(redacted, "", "  ")
	if err != nil {
		return err
	}
	fmt.Fprintf(w, "\n%s\n", string(enc))

	fmt.Fprintf(w, "\n== startup sequence ==\n")
	fmt.Fprintf(w, "1. verify database reachable\n")
	fmt.Fprintf(w, "2. distribute run-config to hosts: %s\n", strings.Join(r.RuntimeHosts(), ", "))
	fmt.Fprintf(w, "3. start BH instances in parallel; wait listen-ready\n")
	fmt.Fprintf(w, "4. start MEE instances in parallel; wait listen-ready\n")
	fmt.Fprintf(w, "5. wait service-ready (.service-ready) on all BH and MEE\n")
	if r.UsesStandalone() {
		fmt.Fprintf(w, "6. start standalone Driver; wait Trade-Cleanup marker\n")
		fmt.Fprintf(w, "7. wait standalone Driver completion\n")
	} else {
		fmt.Fprintf(w, "6. start DM; wait Trade-Cleanup marker in stdout\n")
		fmt.Fprintf(w, "7. start CE instances in parallel\n")
		if waitCE {
			fmt.Fprintf(w, "8. wait all CE instances to exit (duration_sec=%d + grace)\n", r.EffectiveDurationSec())
			fmt.Fprintf(w, "9. stop sequence: CE -> drain -> DM -> MEE -> BH -> collect\n")
		}
	}
	if !waitCE && !r.UsesStandalone() {
		fmt.Fprintf(w, "8. update run-state to running (return without waiting for CE)\n")
	}

	instances, err := argv.BuildAll(r)
	if err != nil {
		return err
	}
	fmt.Fprintf(w, "\n== per-instance argv ==\n")
	for _, inst := range instances {
		fmt.Fprintf(w, "\n[%s %s @ %s]\n", inst.Role, inst.Name, inst.Host)
		fmt.Fprintf(w, "  cd %s\n", r.Paths.RemoteRoot)
		fmt.Fprintf(w, "  %s\n", redact.String(inst.Command))
		if inst.ReadyFile != "" {
			fmt.Fprintf(w, "  ready-file: %s\n", inst.ReadyFile)
		}
	}
	_ = raw
	return nil
}

func writeStop(w io.Writer, r *config.ResolvedProfile) error {
	fmt.Fprintf(w, "plan stop profile=%s\n", r.Name)
	if r.UsesStandalone() {
		fmt.Fprintf(w, "1. SIGTERM standalone Driver\n")
	} else {
		fmt.Fprintf(w, "1. SIGTERM CE instances\n")
	}
	fmt.Fprintf(w, "2. wait mee_drain=%s with DM/MEE/BH running\n", r.Timeouts.MEEDrain)
	if !r.UsesStandalone() {
		fmt.Fprintf(w, "3. SIGTERM DM\n")
		fmt.Fprintf(w, "4. SIGTERM MEE instances\n")
		fmt.Fprintf(w, "5. SIGTERM BH instances\n")
	} else {
		fmt.Fprintf(w, "3. SIGTERM MEE instances\n")
		fmt.Fprintf(w, "4. SIGTERM BH instances\n")
	}
	return nil
}

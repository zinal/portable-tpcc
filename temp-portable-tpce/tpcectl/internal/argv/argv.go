package argv

import (
	"fmt"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

// InstanceArgv is the argv for one orchestrated process.
type InstanceArgv struct {
	Role     string
	Name     string
	Host     string
	Binary   string
	Args     []string
	Command  string
	Output   string
	ReadyFile string
}

// BuildAll returns argv for every runtime instance (spec-orchestrator §9.3).
func BuildAll(r *config.ResolvedProfile) ([]InstanceArgv, error) {
	if r == nil {
		return nil, fmt.Errorf("profile is nil")
	}

	runConfig := r.RemoteRunConfigPath()
	out := make([]InstanceArgv, 0, len(r.BH)+len(r.MEE)+len(r.CE)+2)

	for _, bh := range r.BH {
		ready := filepath.ToSlash(filepath.Join(bh.Output, ".service-ready"))
		args := []string{
			"--run-config", runConfig,
			"--instance", bh.Name,
			"-l", strconv.Itoa(bh.Listen),
			"-o", filepath.ToSlash(bh.Output),
			"--ready-file", ready,
			"--pool-init-timeout", strconv.Itoa(poolInitSecBH(r.Timeouts.Ready)),
		}
		out = append(out, InstanceArgv{
			Role: "bh", Name: bh.Name, Host: bh.Host,
			Binary: "bin/BrokerageHouseMain-pgsql.exe", Args: args,
			Command: formatCommand("bin/BrokerageHouseMain-pgsql.exe", args),
			Output: filepath.ToSlash(bh.Output), ReadyFile: ready,
		})
	}

	for _, mee := range r.MEE {
		ready := filepath.ToSlash(filepath.Join(mee.Output, ".service-ready"))
		args := []string{
			"--run-config", runConfig,
			"--instance", mee.Name,
			"-l", strconv.Itoa(mee.Listen),
			"-U", strconv.Itoa(mee.UniqueID),
			"-o", filepath.ToSlash(mee.Output),
			"--ready-file", ready,
			"--pool-init-timeout", strconv.Itoa(poolInitSecDefault(r.Timeouts.Ready)),
		}
		out = append(out, InstanceArgv{
			Role: "mee", Name: mee.Name, Host: mee.Host,
			Binary: "bin/MarketExchange.exe", Args: args,
			Command: formatCommand("bin/MarketExchange.exe", args),
			Output: filepath.ToSlash(mee.Output), ReadyFile: ready,
		})
	}

	if r.UsesStandalone() {
		sd := r.StandaloneDriver
		args := []string{
			"--run-config", runConfig,
			"--role", "standalone",
			"--instance", "standalone",
			"-u", strconv.Itoa(sd.Users),
			"--ce-id-base", strconv.Itoa(sd.CEIDBase),
			"-o", filepath.ToSlash(sd.Output),
			"--pool-init-timeout", strconv.Itoa(poolInitSecDefault(r.Timeouts.Ready)),
			"-d", strconv.Itoa(sd.DurationSec),
		}
		out = append(out, InstanceArgv{
			Role: "standalone", Name: "standalone", Host: sd.Host,
			Binary: "bin/Driver.exe", Args: args,
			Command: formatCommand("bin/Driver.exe", args),
			Output: filepath.ToSlash(sd.Output),
		})
		return out, nil
	}

	if r.DM != nil {
		dm := r.DM
		dmDuration := DMDurationSec(r.Timeouts, r.Scale.DurationSec)
		args := []string{
			"--run-config", runConfig,
			"--role", "dm",
			"--instance", dm.Name,
			"-o", filepath.ToSlash(dm.Output),
			"--pool-init-timeout", strconv.Itoa(poolInitSecDefault(r.Timeouts.Ready)),
			"-d", strconv.Itoa(dmDuration),
		}
		out = append(out, InstanceArgv{
			Role: "dm", Name: dm.Name, Host: dm.Host,
			Binary: "bin/Driver.exe", Args: args,
			Command: formatCommand("bin/Driver.exe", args),
			Output: filepath.ToSlash(dm.Output),
		})
	}

	for _, ce := range r.CE {
		args := []string{
			"--run-config", runConfig,
			"--role", "ce",
			"--instance", ce.Name,
			"-u", strconv.Itoa(ce.Users),
			"--ce-id-base", strconv.Itoa(ce.CEIDBase),
			"-o", filepath.ToSlash(ce.Output),
			"--pool-init-timeout", strconv.Itoa(poolInitSecDefault(r.Timeouts.Ready)),
		}
		if ce.Partition != nil {
			args = append(args,
				"--ce-start-id", strconv.Itoa(ce.Partition.StartID),
				"--ce-part-count", strconv.Itoa(ce.Partition.Count),
				"--ce-part-percent", strconv.Itoa(ce.Partition.Percent),
			)
		}
		out = append(out, InstanceArgv{
			Role: "ce", Name: ce.Name, Host: ce.Host,
			Binary: "bin/Driver.exe", Args: args,
			Command: formatCommand("bin/Driver.exe", args),
			Output: filepath.ToSlash(ce.Output),
		})
	}

	return out, nil
}

// DMDurationSec computes conservative DM -d (spec-orchestrator §9.3).
func DMDurationSec(t config.TimeoutsConfig, durationSec int) int {
	total := t.CleanupWait + time.Duration(durationSec)*time.Second +
		t.CECompletionGrace + t.MEEDrain + t.StopGrace
	sec := int(total.Seconds())
	if total%time.Second != 0 {
		sec++
	}
	return sec
}

func poolInitSecBH(ready time.Duration) int {
	return int((2 * ready).Seconds())
}

func poolInitSecDefault(ready time.Duration) int {
	return int(ready.Seconds())
}

func formatCommand(binary string, args []string) string {
	parts := make([]string, 0, len(args)+1)
	parts = append(parts, shellQuote(binary))
	for _, a := range args {
		parts = append(parts, shellQuote(a))
	}
	return strings.Join(parts, " ")
}

func shellQuote(s string) string {
	if s == "" {
		return "''"
	}
	if !strings.ContainsAny(s, " \t\n'\"\\$`") {
		return s
	}
	return "'" + strings.ReplaceAll(s, "'", "'\\''") + "'"
}

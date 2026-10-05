package config

import (
	"fmt"
	"strconv"
	"strings"

	"portable-tpcc/mind/internal/profile"
)

// MetricsWorker is one load process that can expose Prometheus metrics.
type MetricsWorker struct {
	Instance string
	Host     string
}

// MetricsTarget is the scrape address of one worker on its host.
// Port is base + Index, and Index restarts at 0 for each host.
type MetricsTarget struct {
	Instance string
	Host     string
	Index    int
	Port     int
}

// EffectiveMetricsBase returns the listen-port base. Non-positive values
// use profile.DefaultMetricsPort.
func EffectiveMetricsBase(configured int) int {
	if configured <= 0 {
		return profile.DefaultMetricsPort
	}
	return configured
}

// AssignMetricsTargets numbers worker processes per host in input order.
// The first worker on a host is index 0 and listens on base; the next on
// that same host listens on base+1, and so on. A different host starts again
// at 0.
func AssignMetricsTargets(workers []MetricsWorker, base int) ([]MetricsTarget, error) {
	base = EffectiveMetricsBase(base)
	if base < 1 || base > 65535 {
		return nil, fmt.Errorf("metrics port base %d is outside 1..65535", base)
	}
	next := map[string]int{}
	out := make([]MetricsTarget, 0, len(workers))
	for _, w := range workers {
		index := next[w.Host]
		port := base + index
		if port > 65535 {
			return nil, fmt.Errorf("metrics port %d for %s on %s exceeds 65535", port, w.Instance, w.Host)
		}
		out = append(out, MetricsTarget{
			Instance: w.Instance,
			Host:     w.Host,
			Index:    index,
			Port:     port,
		})
		next[w.Host] = index + 1
	}
	return out, nil
}

func scrapeHost(host string) string {
	if strings.Count(host, ":") >= 2 && !strings.HasPrefix(host, "[") {
		return "[" + host + "]"
	}
	return host
}

// PrometheusScrapeFragment is a Prometheus config snippet that scrapes the
// worker /metrics endpoints for one profile. It includes a global scrape
// interval of 15s so rate()[1m] has four samples.
func PrometheusScrapeFragment(profileName, dbms string, targets []MetricsTarget) string {
	var b strings.Builder
	b.WriteString("# Prometheus scrape fragment for portable-tpcc workers.\n")
	b.WriteString("# Listen port on a host is runtime.metrics_port + process index (0, 1, 2, ...).\n")
	b.WriteString("# Counters and histograms are cumulative for the worker process.\n")
	b.WriteString("# tpmC: sum(rate(tpcc_transactions_total{type=\"new_order\",result=\"success\"}[1m])) * 60\n")
	b.WriteString("global:\n")
	b.WriteString("  scrape_interval: 15s\n")
	b.WriteString("  scrape_timeout: 15s\n")
	b.WriteString("  evaluation_interval: 15s\n")
	b.WriteString("scrape_configs:\n")
	b.WriteString("  - job_name: portable-tpcc\n")
	b.WriteString("    metrics_path: /metrics\n")
	b.WriteString("    scheme: http\n")
	b.WriteString("    static_configs:\n")
	if len(targets) == 0 {
		b.WriteString("      []\n")
		return b.String()
	}
	for _, t := range targets {
		target := scrapeHost(t.Host) + ":" + strconv.Itoa(t.Port)
		b.WriteString("      - targets:\n")
		b.WriteString("          - " + strconv.Quote(target) + "\n")
		b.WriteString("        labels:\n")
		b.WriteString("          profile: " + strconv.Quote(profileName) + "\n")
		b.WriteString("          dbms: " + strconv.Quote(dbms) + "\n")
		b.WriteString("          worker: " + strconv.Quote(t.Instance) + "\n")
		b.WriteString("          metrics_process: " + strconv.Quote(strconv.Itoa(t.Index)) + "\n")
	}
	return b.String()
}

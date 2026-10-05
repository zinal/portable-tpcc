package config

import (
	"fmt"
	"sort"
	"strings"
	"testing"

	"portable-tpcc/mind/internal/profile"
)

func TestDefaultMetricsPortBelowEphemeralRange(t *testing.T) {
	t.Parallel()
	if profile.DefaultMetricsPort < 1024 || profile.DefaultMetricsPort >= 32768 {
		t.Fatalf("DefaultMetricsPort=%d, want 1024..32767 (below Linux ip_local_port_range)", profile.DefaultMetricsPort)
	}
}

func TestAssignMetricsTargetsPerHost(t *testing.T) {
	t.Parallel()
	workers := []MetricsWorker{
		{Instance: "a", Host: "h1"},
		{Instance: "b", Host: "h2"},
		{Instance: "c", Host: "h1"},
		{Instance: "d", Host: "h2"},
	}
	got, err := AssignMetricsTargets(workers, 0)
	if err != nil {
		t.Fatal(err)
	}
	base := profile.DefaultMetricsPort
	want := []int{base, base, base + 1, base + 1}
	if len(got) != len(want) {
		t.Fatalf("len=%d", len(got))
	}
	for i, port := range want {
		if got[i].Port != port || got[i].Instance != workers[i].Instance {
			t.Fatalf("target[%d]=%+v, want port %d", i, got[i], port)
		}
		if got[i].Index != port-base {
			t.Fatalf("index[%d]=%d", i, got[i].Index)
		}
	}
}

func TestAssignMetricsTargetsRepeatedHosts(t *testing.T) {
	t.Parallel()
	var workers []MetricsWorker
	for round := 0; round < 2; round++ {
		for i := 1; i <= 12; i++ {
			host := fmt.Sprintf("ob-runner-%d", i)
			workers = append(workers, MetricsWorker{
				Instance: fmt.Sprintf("%s-%d", host, round+3),
				Host:     host,
			})
		}
	}
	got, err := AssignMetricsTargets(workers, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 24 {
		t.Fatalf("len=%d", len(got))
	}
	seen := map[string]map[int]string{}
	for _, tgot := range got {
		wantPort := profile.DefaultMetricsPort
		if strings.HasSuffix(tgot.Instance, "-4") {
			wantPort++
		}
		if tgot.Port != wantPort {
			t.Fatalf("%s on %s port=%d, want %d", tgot.Instance, tgot.Host, tgot.Port, wantPort)
		}
		if seen[tgot.Host] == nil {
			seen[tgot.Host] = map[int]string{}
		}
		if prev, ok := seen[tgot.Host][tgot.Port]; ok {
			t.Fatalf("host %s port %d assigned to %s and %s", tgot.Host, tgot.Port, prev, tgot.Instance)
		}
		seen[tgot.Host][tgot.Port] = tgot.Instance
	}

	sorted := append([]MetricsWorker(nil), workers...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].Instance < sorted[j].Instance })
	gotSorted, err := AssignMetricsTargets(sorted, 0)
	if err != nil {
		t.Fatal(err)
	}
	ports := map[string]int{}
	for _, tgot := range got {
		ports[tgot.Instance] = tgot.Port
	}
	for _, tgot := range gotSorted {
		if ports[tgot.Instance] != tgot.Port {
			t.Fatalf("%s port profile-order=%d name-order=%d", tgot.Instance, ports[tgot.Instance], tgot.Port)
		}
	}
}

func TestAssignMetricsTargetsCustomBase(t *testing.T) {
	t.Parallel()
	got, err := AssignMetricsTargets([]MetricsWorker{{Instance: "a", Host: "h"}}, 44000)
	if err != nil {
		t.Fatal(err)
	}
	if got[0].Port != 44000 {
		t.Fatalf("port=%d", got[0].Port)
	}
	if _, err := AssignMetricsTargets([]MetricsWorker{{Instance: "a", Host: "h"}}, 65535); err != nil {
		t.Fatal(err)
	}
	if _, err := AssignMetricsTargets([]MetricsWorker{
		{Instance: "a", Host: "h"},
		{Instance: "b", Host: "h"},
	}, 65535); err == nil {
		t.Fatal("expected overflow")
	}
}

func TestPrometheusScrapeFragment(t *testing.T) {
	t.Parallel()
	targets, err := AssignMetricsTargets([]MetricsWorker{
		{Instance: "w-a", Host: "10.0.0.1"},
		{Instance: "w-b", Host: "10.0.0.1"},
		{Instance: "w-c", Host: "10.0.0.2"},
	}, profile.DefaultMetricsPort)
	if err != nil {
		t.Fatal(err)
	}
	text := PrometheusScrapeFragment("bench", "pgsql", targets)
	for _, want := range []string{
		"job_name: portable-tpcc",
		"metrics_path: /metrics",
		`sum(rate(tpcc_transactions_total{type="new_order",result="success"}[1m])) * 60`,
		`"10.0.0.1:14380"`,
		`"10.0.0.1:14381"`,
		`"10.0.0.2:14380"`,
		`profile: "bench"`,
		`dbms: "pgsql"`,
		`worker: "w-b"`,
		`metrics_process: "1"`,
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("fragment missing %q\n%s", want, text)
		}
	}
}

func TestWorkerArgvMetricsPort(t *testing.T) {
	t.Parallel()
	got := WorkerArgv("run-config.json", "w", "2020-01-01T00:00:00Z", nil, nil, 43801)
	if len(got) == 0 || got[len(got)-1] != "--metrics-port=43801" {
		t.Fatalf("argv %v", got)
	}
	plain := WorkerArgv("run-config.json", "w", "", nil, nil, 0)
	for _, arg := range plain {
		if strings.Contains(arg, "metrics-port") {
			t.Fatalf("disabled argv %v", plain)
		}
	}
}

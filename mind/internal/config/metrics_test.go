package config

import (
	"strings"
	"testing"
)

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
	want := []int{43800, 43800, 43801, 43801}
	if len(got) != len(want) {
		t.Fatalf("len=%d", len(got))
	}
	for i, port := range want {
		if got[i].Port != port || got[i].Instance != workers[i].Instance {
			t.Fatalf("target[%d]=%+v, want port %d", i, got[i], port)
		}
		if got[i].Index != port-43800 {
			t.Fatalf("index[%d]=%d", i, got[i].Index)
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
	}, 43800)
	if err != nil {
		t.Fatal(err)
	}
	text := PrometheusScrapeFragment("bench", "pgsql", targets)
	for _, want := range []string{
		"job_name: portable-tpcc",
		"metrics_path: /metrics",
		`sum(rate(tpcc_transactions_total{type="new_order",result="success"}[1m])) * 60`,
		`"10.0.0.1:43800"`,
		`"10.0.0.1:43801"`,
		`"10.0.0.2:43800"`,
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

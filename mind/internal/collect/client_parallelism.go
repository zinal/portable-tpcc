package collect

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"

	"portable-tpcc/mind/internal/canonical"
	"portable-tpcc/mind/internal/config"
)

// ClientParallelism captures workload-client concurrency for a run.
// threads_per_worker / max_inflight_per_worker are the profile-level values
// materialized into each worker assignment (uniform under BuildRunConfig).
type ClientParallelism struct {
	SchemaVersion        int                       `json:"schema_version"`
	Clients              int                       `json:"clients"`
	ThreadsPerWorker     int                       `json:"threads_per_worker"`
	MaxInflightPerWorker int                       `json:"max_inflight_per_worker"`
	Workers              []ClientWorkerParallelism `json:"workers"`
}

// ClientWorkerParallelism is per-worker client concurrency.
type ClientWorkerParallelism struct {
	Instance    string `json:"instance"`
	Host        string `json:"host,omitempty"`
	Threads     int    `json:"threads"`
	MaxInflight int    `json:"max_inflight"`
}

// ClientParallelismFileName is written under results/<run_id>/ during collect.
const ClientParallelismFileName = "client-parallelism.json"

// FromWorkerAssignments builds ClientParallelism from run-config assignments.
func FromWorkerAssignments(assigns []config.WorkerAssignmentJSON) ClientParallelism {
	workers := make([]ClientWorkerParallelism, 0, len(assigns))
	for _, a := range assigns {
		workers = append(workers, ClientWorkerParallelism{
			Instance:    a.Instance,
			Host:        a.Host,
			Threads:     a.Threads,
			MaxInflight: a.MaxInflight,
		})
	}
	return finalizeClientParallelism(workers)
}

// FromWorkerResultAssignments builds ClientParallelism from per-worker
// result.json assignment objects (instance -> assignment map).
func FromWorkerResultAssignments(byInstance map[string]map[string]interface{}) ClientParallelism {
	names := make([]string, 0, len(byInstance))
	for name := range byInstance {
		names = append(names, name)
	}
	sort.Strings(names)
	workers := make([]ClientWorkerParallelism, 0, len(names))
	for _, name := range names {
		assign := byInstance[name]
		w := ClientWorkerParallelism{Instance: name}
		if assign != nil {
			if inst, _ := assign["instance"].(string); inst != "" {
				w.Instance = inst
			}
			if host, _ := assign["host"].(string); host != "" {
				w.Host = host
			}
			if n, ok := jsonInt(assign["threads"]); ok {
				w.Threads = n
			}
			if n, ok := jsonInt(assign["max_inflight"]); ok {
				w.MaxInflight = n
			}
		}
		workers = append(workers, w)
	}
	return finalizeClientParallelism(workers)
}

func finalizeClientParallelism(workers []ClientWorkerParallelism) ClientParallelism {
	out := ClientParallelism{
		SchemaVersion: 1,
		Clients:       len(workers),
		Workers:       workers,
	}
	if len(workers) == 0 {
		return out
	}
	// Profile materialization is normally uniform across workers; use the
	// first assignment as threads_per_worker / max_inflight_per_worker.
	// Per-worker values remain in Workers when they differ.
	out.ThreadsPerWorker = workers[0].Threads
	out.MaxInflightPerWorker = workers[0].MaxInflight
	return out
}

func jsonInt(v interface{}) (int, bool) {
	switch n := v.(type) {
	case float64:
		if n != float64(int(n)) {
			return 0, false
		}
		return int(n), true
	case json.Number:
		i, err := n.Int64()
		if err != nil {
			return 0, false
		}
		return int(i), true
	case int:
		return n, true
	case int64:
		return int(n), true
	default:
		return 0, false
	}
}

// WriteClientParallelism atomically writes client-parallelism.json and returns
// a control-file payload entry for the collection manifest.
func WriteClientParallelism(resultRoot, runID string, cp ClientParallelism) (ArtifactPayloadEntry, error) {
	dir := filepath.Join(resultRoot, runID)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return ArtifactPayloadEntry{}, err
	}
	data, err := json.MarshalIndent(cp, "", "  ")
	if err != nil {
		return ArtifactPayloadEntry{}, err
	}
	data = append(data, '\n')
	tmp := filepath.Join(dir, ClientParallelismFileName+".tmp")
	final := filepath.Join(dir, ClientParallelismFileName)
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return ArtifactPayloadEntry{}, err
	}
	if err := os.Rename(tmp, final); err != nil {
		return ArtifactPayloadEntry{}, err
	}
	sha, err := canonical.SHA256File(final)
	if err != nil {
		return ArtifactPayloadEntry{}, err
	}
	info, err := os.Stat(final)
	if err != nil {
		return ArtifactPayloadEntry{}, err
	}
	return ArtifactPayloadEntry{
		Path:   ClientParallelismFileName,
		Size:   info.Size(),
		SHA256: sha,
	}, nil
}

// ReadClientParallelism loads client-parallelism.json when present.
func ReadClientParallelism(resultRoot, runID string) (ClientParallelism, error) {
	path := filepath.Join(resultRoot, runID, ClientParallelismFileName)
	data, err := os.ReadFile(path)
	if err != nil {
		return ClientParallelism{}, err
	}
	var cp ClientParallelism
	if err := json.Unmarshal(data, &cp); err != nil {
		return ClientParallelism{}, fmt.Errorf("%s: %w", ClientParallelismFileName, err)
	}
	return cp, nil
}

// ToMap returns a JSON-object form suitable for aggregate settings.
func (cp ClientParallelism) ToMap() map[string]interface{} {
	workers := make([]map[string]interface{}, 0, len(cp.Workers))
	for _, w := range cp.Workers {
		entry := map[string]interface{}{
			"instance":     w.Instance,
			"threads":      w.Threads,
			"max_inflight": w.MaxInflight,
		}
		if w.Host != "" {
			entry["host"] = w.Host
		}
		workers = append(workers, entry)
	}
	return map[string]interface{}{
		"schema_version":          cp.SchemaVersion,
		"clients":                 cp.Clients,
		"threads_per_worker":      cp.ThreadsPerWorker,
		"max_inflight_per_worker": cp.MaxInflightPerWorker,
		"workers":                 workers,
	}
}

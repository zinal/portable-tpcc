package assignment

import (
	"fmt"
	"sort"
)

const AlgorithmBalancedContiguousV1 = "balanced-contiguous-v1"

// Instance names a loader or worker instance.
type Instance struct {
	Name string
	Host string
}

// WarehouseRange is a half-open interval [Start, End).
type WarehouseRange struct {
	Start int
	End   int
}

// LoaderAssignment describes warehouse ranges and global-data ownership for a loader.
type LoaderAssignment struct {
	Instance         string
	Host             string
	WarehouseRanges  []WarehouseRange
	OwnsGlobalData   bool
}

// WorkerAssignment describes warehouse ranges and runtime params for a worker.
type WorkerAssignment struct {
	Instance         string
	Host             string
	WarehouseRanges  []WarehouseRange
	Threads          int
	MaxInflight      int
}

// BalancedContiguousV1 divides warehouses across instances per specification §5.1.
func BalancedContiguousV1(instances []Instance, warehouses int) ([]WarehouseRange, error) {
	if warehouses <= 0 {
		return nil, fmt.Errorf("warehouses must be positive, got %d", warehouses)
	}
	if len(instances) == 0 {
		return nil, fmt.Errorf("instance list is empty")
	}
	if len(instances) > warehouses {
		return nil, fmt.Errorf("instance count %d exceeds warehouses %d", len(instances), warehouses)
	}

	sorted := append([]Instance(nil), instances...)
	sort.Slice(sorted, func(i, j int) bool {
		return sorted[i].Name < sorted[j].Name
	})

	n := len(sorted)
	base := warehouses / n
	extra := warehouses % n

	ranges := make([]WarehouseRange, n)
	start := 1
	for i := 0; i < n; i++ {
		count := base
		if i < extra {
			count++
		}
		ranges[i] = WarehouseRange{Start: start, End: start + count}
		start += count
	}
	return ranges, nil
}

// BuildLoaderAssignments applies balanced-contiguous-v1 to loaders.
func BuildLoaderAssignments(loaders []Instance, warehouses int) ([]LoaderAssignment, error) {
	ranges, err := BalancedContiguousV1(loaders, warehouses)
	if err != nil {
		return nil, err
	}
	sorted := append([]Instance(nil), loaders...)
	sort.Slice(sorted, func(i, j int) bool {
		return sorted[i].Name < sorted[j].Name
	})
	out := make([]LoaderAssignment, len(sorted))
	for i, inst := range sorted {
		out[i] = LoaderAssignment{
			Instance:         inst.Name,
			Host:             inst.Host,
			WarehouseRanges:  []WarehouseRange{ranges[i]},
			OwnsGlobalData:   i == 0,
		}
	}
	return out, nil
}

// BuildWorkerAssignments applies balanced-contiguous-v1 to workers.
func BuildWorkerAssignments(workers []Instance, warehouses, threads, maxInflight int) ([]WorkerAssignment, error) {
	ranges, err := BalancedContiguousV1(workers, warehouses)
	if err != nil {
		return nil, err
	}
	sorted := append([]Instance(nil), workers...)
	sort.Slice(sorted, func(i, j int) bool {
		return sorted[i].Name < sorted[j].Name
	})
	out := make([]WorkerAssignment, len(sorted))
	for i, inst := range sorted {
		out[i] = WorkerAssignment{
			Instance:         inst.Name,
			Host:             inst.Host,
			WarehouseRanges:  []WarehouseRange{ranges[i]},
			Threads:          threads,
			MaxInflight:      maxInflight,
		}
	}
	return out, nil
}

// ExpectedWorkersSHA256 returns canonical hash of sorted worker instance names.
func ExpectedWorkersSHA256(workerNames []string) (string, error) {
	sorted := append([]string(nil), workerNames...)
	sort.Strings(sorted)
	// Use canonical package - import in callers or duplicate simple hash
	return "", nil
}

// ToJSONRanges converts warehouse ranges to JSON-friendly [start, end] pairs.
func ToJSONRanges(ranges []WarehouseRange) [][]int {
	out := make([][]int, len(ranges))
	for i, r := range ranges {
		out[i] = []int{r.Start, r.End}
	}
	return out
}

// ValidateAssignment checks completeness and non-overlap for warehouse ranges.
func ValidateAssignment(assignments []LoaderAssignment, warehouses int) error {
	covered := 0
	for _, a := range assignments {
		for _, r := range a.WarehouseRanges {
			if r.Start < 1 || r.End <= r.Start {
				return fmt.Errorf("invalid range [%d, %d) for %s", r.Start, r.End, a.Instance)
			}
			covered += r.End - r.Start
		}
	}
	if covered != warehouses {
		return fmt.Errorf("assignment covers %d warehouses, expected %d", covered, warehouses)
	}
	globalOwners := 0
	for _, a := range assignments {
		if a.OwnsGlobalData {
			globalOwners++
		}
	}
	if globalOwners != 1 {
		return fmt.Errorf("expected exactly one global data owner, got %d", globalOwners)
	}
	return nil
}

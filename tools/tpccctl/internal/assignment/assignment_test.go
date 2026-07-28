package assignment_test

import (
	"testing"

	"portable-tpcc/tools/tpccctl/internal/assignment"
)

func TestBalancedContiguousV1_even(t *testing.T) {
	inst := []assignment.Instance{
		{Name: "loader-a", Host: "h1"},
		{Name: "loader-b", Host: "h2"},
	}
	ranges, err := assignment.BalancedContiguousV1(inst, 200)
	if err != nil {
		t.Fatal(err)
	}
	if len(ranges) != 2 {
		t.Fatalf("expected 2 ranges, got %d", len(ranges))
	}
	if ranges[0].Start != 1 || ranges[0].End != 101 {
		t.Fatalf("loader-a range: got [%d,%d)", ranges[0].Start, ranges[0].End)
	}
	if ranges[1].Start != 101 || ranges[1].End != 201 {
		t.Fatalf("loader-b range: got [%d,%d)", ranges[1].Start, ranges[1].End)
	}
}

func TestBalancedContiguousV1_uneven(t *testing.T) {
	inst := []assignment.Instance{
		{Name: "w-a", Host: "h1"},
		{Name: "w-b", Host: "h2"},
		{Name: "w-c", Host: "h3"},
	}
	ranges, err := assignment.BalancedContiguousV1(inst, 10)
	if err != nil {
		t.Fatal(err)
	}
	total := 0
	for _, r := range ranges {
		total += r.End - r.Start
	}
	if total != 10 {
		t.Fatalf("covered %d warehouses, want 10", total)
	}
	// First instance gets extra warehouse: 4,3,3
	if ranges[0].End-ranges[0].Start != 4 {
		t.Fatalf("first range size %d, want 4", ranges[0].End-ranges[0].Start)
	}
}

func TestBalancedContiguousV1_orderIndependent(t *testing.T) {
	a := []assignment.Instance{{Name: "loader-b", Host: "h2"}, {Name: "loader-a", Host: "h1"}}
	b := []assignment.Instance{{Name: "loader-a", Host: "h1"}, {Name: "loader-b", Host: "h2"}}
	ra, err := assignment.BalancedContiguousV1(a, 6)
	if err != nil {
		t.Fatal(err)
	}
	rb, err := assignment.BalancedContiguousV1(b, 6)
	if err != nil {
		t.Fatal(err)
	}
	if ra[0] != rb[0] || ra[1] != rb[1] {
		t.Fatalf("order changed assignment: %v vs %v", ra, rb)
	}
}

func TestBuildLoaderAssignments_globalOwner(t *testing.T) {
	loaders := []assignment.Instance{{Name: "loader-a", Host: "h1"}, {Name: "loader-b", Host: "h2"}}
	assign, err := assignment.BuildLoaderAssignments(loaders, 4)
	if err != nil {
		t.Fatal(err)
	}
	if !assign[0].OwnsGlobalData {
		t.Fatal("first sorted loader must own global data")
	}
	if assign[1].OwnsGlobalData {
		t.Fatal("second loader must not own global data")
	}
	if err := assignment.ValidateAssignment(assign, 4); err != nil {
		t.Fatal(err)
	}
}

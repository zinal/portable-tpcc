package config_test

import (
	"testing"

	"portable-tpcc/mind/internal/config"
	"portable-tpcc/mind/internal/profile"
)

func intPtr(v int) *int {
	return &v
}

func TestResolveWorkload_remoteWarehouseDefaults(t *testing.T) {
	got := config.ResolveWorkload(profile.Workload{})
	want := config.DefaultWorkload()
	if got.RemoteWarehousePercent != want.RemoteWarehousePercent {
		t.Fatalf("defaults=%+v, want %+v", got.RemoteWarehousePercent, want.RemoteWarehousePercent)
	}
	if want.RemoteWarehousePercent.NewOrder != 1 || want.RemoteWarehousePercent.Payment != 15 {
		t.Fatalf("TPC-C defaults=%+v, want new_order=1 payment=15", want.RemoteWarehousePercent)
	}
}

func TestResolveWorkload_remoteWarehouseExplicitZero(t *testing.T) {
	got := config.ResolveWorkload(profile.Workload{
		RemoteWarehousePercent: profile.RemoteWarehousePercent{
			NewOrder: intPtr(0),
			Payment:  intPtr(0),
		},
	})
	if got.RemoteWarehousePercent.NewOrder != 0 || got.RemoteWarehousePercent.Payment != 0 {
		t.Fatalf("explicit 0 was lost: %+v", got.RemoteWarehousePercent)
	}
}

func TestResolveWorkload_remoteWarehousePartialOverride(t *testing.T) {
	got := config.ResolveWorkload(profile.Workload{
		RemoteWarehousePercent: profile.RemoteWarehousePercent{
			Payment: intPtr(100),
		},
	})
	if got.RemoteWarehousePercent.NewOrder != 1 {
		t.Fatalf("omitted new_order should stay 1, got %d", got.RemoteWarehousePercent.NewOrder)
	}
	if got.RemoteWarehousePercent.Payment != 100 {
		t.Fatalf("payment=%d, want 100", got.RemoteWarehousePercent.Payment)
	}
}

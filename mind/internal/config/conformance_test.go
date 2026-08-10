package config_test

import (
	"strings"
	"testing"

	"portable-tpcc/mind/internal/config"
)

func conformingRunConfig() *config.RunConfig {
	wl := config.DefaultWorkload()
	return &config.RunConfig{
		Workload: wl,
		Phases: config.PhasesJSON{
			MeasurementMs: config.TPCCMinMeasurementMs,
		},
		Runtime: config.RunRuntime{
			Pacing:                "enabled",
			ThinkTimeDistribution: config.DefaultThinkTimeDistribution,
		},
	}
}

func TestTPCSettingsDeviations_conformant(t *testing.T) {
	devs := config.TPCSettingsDeviations(conformingRunConfig())
	if len(devs) != 0 {
		t.Fatalf("expected no deviations, got %#v", devs)
	}
}

func TestTPCSettingsDeviations_reportsAllClasses(t *testing.T) {
	rc := conformingRunConfig()
	rc.Workload.TerminalsPerWarehouse = 20
	rc.Workload.TransactionMix.Payment = 30
	rc.Workload.KeyingTimeMs.NewOrder = 1000
	rc.Workload.ThinkTimeMs.Delivery = 1000
	rc.Runtime.Pacing = "disabled"
	rc.Runtime.ThinkTimeDistribution = "compatibility"
	rc.Phases.MeasurementMs = 30 * 60 * 1000

	devs := config.TPCSettingsDeviations(rc)
	wantSubs := []string{
		"terminals_per_warehouse=20",
		"Stock-Level home",
		"transaction_mix.payment",
		"keying_time_ms.new_order=1000",
		"think_time_ms.delivery=1000",
		`pacing="disabled"`,
		`think_time_distribution="compatibility"`,
		"phases.measurement=",
	}
	for _, sub := range wantSubs {
		found := false
		for _, d := range devs {
			if strings.Contains(d, sub) {
				found = true
				break
			}
		}
		if !found {
			t.Fatalf("missing deviation containing %q in %#v", sub, devs)
		}
	}
}

func TestTPCSettingsDeviations_emptyPacingIsEnabled(t *testing.T) {
	rc := conformingRunConfig()
	rc.Runtime.Pacing = ""
	if devs := config.TPCSettingsDeviations(rc); len(devs) != 0 {
		t.Fatalf("empty pacing should resolve to enabled, got %#v", devs)
	}
}

func TestTPCSettingsDeviations_zeroMeasurementIsNonConformant(t *testing.T) {
	rc := conformingRunConfig()
	rc.Phases.MeasurementMs = 0
	devs := config.TPCSettingsDeviations(rc)
	found := false
	for _, d := range devs {
		if strings.Contains(d, "phases.measurement=0ms") {
			found = true
			break
		}
	}
	if !found {
		t.Fatalf("expected zero measurement deviation, got %#v", devs)
	}
}

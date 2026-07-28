package runtimeconfig

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"path/filepath"
	"time"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/endpoints"
)

// Document is the global run-config.json written by tpcectl (schema v1).
type Document struct {
	SchemaVersion int    `json:"schema_version"`
	RunID         string `json:"run_id"`
	ProfileName   string `json:"profile_name"`
	GeneratedAt   string `json:"generated_at"`
	Paths         struct {
		Data string `json:"data"`
	} `json:"paths"`
	Scale struct {
		ActiveCustomers     int `json:"active_customers"`
		ConfiguredCustomers int `json:"configured_customers"`
		ScaleFactor         int `json:"scale_factor"`
		InitialTradeDays    int `json:"initial_trade_days"`
		DurationSec         int `json:"duration_sec"`
	} `json:"scale"`
	Database struct {
		Host        string `json:"host"`
		Port        int    `json:"port"`
		Name        string `json:"name"`
		User        string `json:"user"`
		SSLMode     string `json:"sslmode"`
		PasswordEnv string `json:"password_env"`
	} `json:"database"`
	ClientSide    bool  `json:"client_side"`
	BaseTimeEpoch int64 `json:"base_time_epoch"`
	EndpointSets  struct {
		BH  []string `json:"bh"`
		MEE []string `json:"mee"`
	} `json:"endpoint_sets"`
}

// BuildOptions controls run-config generation.
type BuildOptions struct {
	Now           time.Time
	BaseTimeEpoch *int64
}

// Build creates a run-config document and deterministic JSON bytes.
func Build(r *config.ResolvedProfile, opts BuildOptions) (Document, []byte, string, error) {
	if r == nil {
		return Document{}, nil, "", fmt.Errorf("profile is nil")
	}

	now := opts.Now
	if now.IsZero() {
		now = time.Now().UTC()
	}

	ep, err := endpoints.Build(r)
	if err != nil {
		return Document{}, nil, "", err
	}

	epoch, preview, err := resolveBaseTimeEpoch(r, opts, now)
	if err != nil {
		return Document{}, nil, "", err
	}
	_ = preview

	dataPath := filepath.ToSlash(filepath.Join(r.Paths.RemoteRoot, "data"))

	doc := Document{
		SchemaVersion: 1,
		RunID:         r.EffectiveRunID,
		ProfileName:   r.Name,
		GeneratedAt:   now.UTC().Format(time.RFC3339),
		ClientSide:    r.Scale.ClientSide,
		BaseTimeEpoch: epoch,
	}
	doc.Paths.Data = dataPath
	doc.Scale.ActiveCustomers = r.Scale.ActiveCustomers
	doc.Scale.ConfiguredCustomers = r.Scale.Customers
	doc.Scale.ScaleFactor = r.Scale.ScaleFactor
	doc.Scale.InitialTradeDays = r.Scale.InitialTradeDays
	doc.Scale.DurationSec = r.Scale.DurationSec
	doc.Database.Host = r.DB.Host
	doc.Database.Port = r.DB.Port
	doc.Database.Name = r.DB.Name
	doc.Database.User = r.DB.User
	doc.Database.SSLMode = r.DB.SSLMode
	doc.Database.PasswordEnv = r.DB.PasswordEnv
	doc.EndpointSets.BH = ep.BH
	doc.EndpointSets.MEE = ep.MEE

	raw, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		return Document{}, nil, "", fmt.Errorf("marshal run-config: %w", err)
	}
	raw = append(raw, '\n')

	sum := sha256.Sum256(raw)
	return doc, raw, hex.EncodeToString(sum[:]), nil
}

// BaseTimePreview describes how base_time_epoch was chosen for plan output.
type BaseTimePreview struct {
	Value     int64
	Explicit  bool
	Formula   string
}

func resolveBaseTimeEpoch(r *config.ResolvedProfile, opts BuildOptions, now time.Time) (int64, BaseTimePreview, error) {
	if opts.BaseTimeEpoch != nil {
		return *opts.BaseTimeEpoch, BaseTimePreview{Value: *opts.BaseTimeEpoch, Explicit: true}, nil
	}
	if r.BaseTimeEpoch != nil {
		return *r.BaseTimeEpoch, BaseTimePreview{Value: *r.BaseTimeEpoch, Explicit: true}, nil
	}

	lead := int64(r.Timeouts.ConfigDistribute.Seconds()) +
		2*int64(r.Timeouts.Ready.Seconds()) +
		int64(r.BaseTimeLeadSec)
	epoch := now.UTC().Unix() + lead
	formula := "now_utc + config_distribute + 2*ready + base_time_lead_sec"
	return epoch, BaseTimePreview{Value: epoch, Formula: formula}, nil
}

// PreviewBaseTime returns epoch selection metadata without building the full document.
func PreviewBaseTime(r *config.ResolvedProfile, opts BuildOptions) (BaseTimePreview, error) {
	now := opts.Now
	if now.IsZero() {
		now = time.Now().UTC()
	}
	epoch, preview, err := resolveBaseTimeEpoch(r, opts, now)
	if err != nil {
		return BaseTimePreview{}, err
	}
	preview.Value = epoch
	return preview, nil
}

// Redact returns a copy of the document safe for plan output (passwords already absent).
func Redact(doc Document) Document {
	return doc
}

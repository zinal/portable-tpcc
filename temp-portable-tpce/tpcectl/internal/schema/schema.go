package schema

import (
	"context"
	"fmt"

	"github.com/jackc/pgx/v5"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/database"
)

// Options controls schema application (spec-orchestrator §11.1).
type Options struct {
	Recreate bool
	DryRun   bool
	Verbose  bool
}

// Run applies DDL from paths.local_sql to the profile database.
func Run(ctx context.Context, profile *config.ResolvedProfile, opts Options) error {
	if profile == nil {
		return fmt.Errorf("profile is nil")
	}
	sqlDir := profile.Paths.LocalSQL
	if sqlDir == "" {
		return fmt.Errorf("paths.local_sql is required")
	}

	steps, err := buildSteps(profile, true)
	if err != nil {
		return err
	}
	return execute(ctx, profile, steps, opts)
}

// ApplyPostLoad applies deferred indexes and foreign keys after Loader shards (§11.2).
func ApplyPostLoad(ctx context.Context, profile *config.ResolvedProfile, opts Options) error {
	if profile == nil {
		return fmt.Errorf("profile is nil")
	}
	var steps []step
	if applyIndexesAfterLoad(profile) {
		steps = append(steps, step{name: fileCreateIndexes, kind: "indexes"})
	}
	if applyFKsAfterLoad(profile) {
		steps = append(steps, step{name: fileCreateFKs, kind: "foreign keys"})
	}
	if len(steps) == 0 {
		return nil
	}
	return execute(ctx, profile, steps, opts)
}

type step struct {
	name string
	kind string
}

func buildSteps(profile *config.ResolvedProfile, includeTables bool) ([]step, error) {
	var steps []step
	if includeTables {
		file, err := tablesFile(profile.Schema.Mode)
		if err != nil {
			return nil, err
		}
		steps = append(steps, step{name: file, kind: "tables"})
	}
	if applyIndexesDuringSchema(profile) {
		steps = append(steps, step{name: fileCreateIndexes, kind: "indexes"})
	}
	if applyFKsDuringSchema(profile) {
		steps = append(steps, step{name: fileCreateFKs, kind: "foreign keys"})
	}
	return steps, nil
}

func execute(ctx context.Context, profile *config.ResolvedProfile, steps []step, opts Options) error {
	if opts.DryRun {
		for _, s := range steps {
			fmt.Printf("dry-run: would apply %s (%s)\n", s.name, s.kind)
		}
		if opts.Recreate {
			fmt.Println("dry-run: would recreate database objects (DROP SCHEMA public CASCADE)")
		}
		return nil
	}

	conn, err := database.Connect(ctx, profile)
	if err != nil {
		return err
	}
	defer conn.Close(ctx)

	if opts.Recreate {
		if opts.Verbose {
			fmt.Println("schema: recreating public schema")
		}
		if err := recreatePublic(ctx, conn); err != nil {
			return err
		}
	}

	partitions := profile.Schema.Partitions
	if partitions < 2 {
		partitions = 32
	}
	sqlDir := profile.Paths.LocalSQL

	for _, s := range steps {
		if opts.Verbose {
			fmt.Printf("schema: applying %s\n", s.name)
		}
		sql, err := readSQLFile(sqlDir, s.name, partitions)
		if err != nil {
			return err
		}
		if err := execSQL(ctx, conn, sql); err != nil {
			return fmt.Errorf("apply %s: %w", s.name, err)
		}
	}
	return nil
}

func recreatePublic(ctx context.Context, conn *pgx.Conn) error {
	_, err := conn.Exec(ctx, `
DROP SCHEMA IF EXISTS public CASCADE;
CREATE SCHEMA public;
GRANT ALL ON SCHEMA public TO public;
`)
	return err
}

func execSQL(ctx context.Context, conn *pgx.Conn, sql string) error {
	_, err := conn.Exec(ctx, sql)
	return err
}

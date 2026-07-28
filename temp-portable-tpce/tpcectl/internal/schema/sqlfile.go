package schema

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

const (
	fileCreateTables            = "create_tables.sql"
	fileCreateTablesPartitioned = "create_tables_partitioned.sql"
	fileCreateIndexes           = "create_indexes.sql"
	fileCreateFKs               = "create_fks.sql"
)

func tablesFile(mode string) (string, error) {
	switch mode {
	case "base", "":
		return fileCreateTables, nil
	case "partitioned":
		return fileCreateTablesPartitioned, nil
	default:
		return "", fmt.Errorf("unsupported schema.mode %q (want base or partitioned)", mode)
	}
}

func readSQLFile(sqlDir, name string, partitions int) (string, error) {
	path := filepath.Join(sqlDir, name)
	data, err := os.ReadFile(path)
	if err != nil {
		return "", fmt.Errorf("read %s: %w", path, err)
	}
	sql := string(data)
	if name == fileCreateTablesPartitioned {
		sql = preprocessPartitionedSQL(sql, partitions)
	}
	return sql, nil
}

func preprocessPartitionedSQL(sql string, partitions int) string {
	if partitions < 2 {
		partitions = 32
	}
	var b strings.Builder
	for _, line := range strings.Split(sql, "\n") {
		trim := strings.TrimSpace(line)
		if strings.HasPrefix(trim, `\`) {
			continue
		}
		line = strings.ReplaceAll(line, ":partitions", fmt.Sprintf("%d", partitions))
		b.WriteString(line)
		b.WriteByte('\n')
	}
	return b.String()
}

// IndexesDeferred reports whether indexes/FKs run after load (when load shards exist).
func IndexesDeferred(profile *config.ResolvedProfile) bool {
	return profile != nil && len(profile.Load.Shards) > 0
}

func applyIndexesDuringSchema(profile *config.ResolvedProfile) bool {
	return profile.Schema.ApplyIndexes && !IndexesDeferred(profile)
}

func applyFKsDuringSchema(profile *config.ResolvedProfile) bool {
	return profile.Schema.ApplyFKs && !IndexesDeferred(profile)
}

func applyIndexesAfterLoad(profile *config.ResolvedProfile) bool {
	return profile.Schema.ApplyIndexes && IndexesDeferred(profile)
}

func applyFKsAfterLoad(profile *config.ResolvedProfile) bool {
	return profile.Schema.ApplyFKs && IndexesDeferred(profile)
}

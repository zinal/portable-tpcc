#pragma once

#include "schema_options.h"

#include <string>
#include <vector>

namespace NTpcc {

// Resolved physical layout for OceanBase schema DDL.
struct TObSchemaLayout {
    bool UseClusterLayout = false;
    // Replicate DB-wide item to every observer (DUPLICATE_SCOPE = cluster).
    bool DuplicateItem = false;
    int PartitionCount = 1;
};

std::vector<std::string> BuildObCreateStatements(
    const TObSchemaLayout& layout,
    const TObSchemaOptions& options);

void InitSync(
    const std::string& connectionString,
    const std::string& path = {},
    const TObSchemaOptions& options = {});

void CreateIndexes(
    const std::string& connectionString,
    const std::string& path = {},
    bool useLocalIndexes = false,
    int indexParallel = OB_DEFAULT_INDEX_PARALLEL);

void AnalyzeTables(
    const std::string& connectionString,
    const std::string& path = {},
    const TObSchemaOptions& options = {});

// One DBMS_STATS.GATHER_TABLE_STATS statement.
// HASH-partitioned tables other than duplicate `item` are split by partition
// (`p0` …) so each statement stays within session ob_query_timeout.
struct TObGatherTableStatsCall {
    std::string Label;
    std::string Sql;
};

std::vector<TObGatherTableStatsCall> BuildObGatherTableStatsCalls(
    const std::string& database,
    const char* table,
    int degree,
    int hashPartitions);

} // namespace NTpcc

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
// HASH-partitioned tables other than duplicate `item` are one call per
// partition (`p0` …) with degree 1: a HASH partition has a single leader, so
// cluster parallelism is concurrent calls, not intra-partition DOP.
struct TObGatherTableStatsCall {
    std::string Label;
    std::string Sql;
};

// `degree` is the intra-statement DOP for a table that is not HASH-partitioned
// (`item`, or partitioning off). HASH-partition calls always use degree 1.
std::vector<TObGatherTableStatsCall> BuildObGatherTableStatsCalls(
    const std::string& database,
    const char* table,
    int degree,
    int hashPartitions);

// Sessions that gather different HASH partitions of one table at the same time.
// Capped so a warehouse-derived partition count cannot open one session per warehouse.
inline constexpr int OB_MAX_PARALLEL_STATS_GATHERS = 64;

int ObStatsGatherSessionCount(int hashPartitions);

// Aggregate partition column stats. Rows come from
// oceanbase.DBA_PART_COL_STATISTICS, which has PARTITION_NAME.
// DBA_TAB_COL_STATISTICS is global-only and rejects partition_name (error 1054).
std::string BuildObPartitionColumnStatsQuery(
    const std::string& database,
    const char* table);

} // namespace NTpcc

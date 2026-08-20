#pragma once

#include <string>

namespace NTpcc {

inline constexpr const char* OB_PARTITIONING_NONE = "none";
inline constexpr const char* OB_PARTITIONING_TABLEGROUP_HASH = "tablegroup_hash";

// Default CREATE INDEX degree of parallelism (OceanBase PARALLEL n).
inline constexpr int OB_DEFAULT_INDEX_PARALLEL = 4;

struct TObSchemaOptions {
    // -1 = plain tables, 0 = derive from WarehouseCount, >0 = explicit partitions.
    int PartitionCount = 0;
    int WarehouseCount = 1;
    bool EnableForeignKeys = true;
    // DOP for a single CREATE INDEX (PARALLEL n). 1 = serial.
    int IndexParallel = OB_DEFAULT_INDEX_PARALLEL;
};

int ResolveObPartitionCount(const TObSchemaOptions& options);
int ResolveObIndexParallel(const TObSchemaOptions& options);
std::string ObPartitioningStyle(const TObSchemaOptions& options);
std::string ForeignKeysModeLabel(bool enabled);
bool ParseForeignKeysMode(const std::string& value, bool& enabled);

} // namespace NTpcc

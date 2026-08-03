#pragma once

#include <string>

namespace NTpcc {

inline constexpr const char* OB_PARTITIONING_NONE = "none";
inline constexpr const char* OB_PARTITIONING_TABLEGROUP_HASH = "tablegroup_hash";

struct TObSchemaOptions {
    // -1 = plain tables, 0 = derive from WarehouseCount, >0 = explicit partitions.
    int PartitionCount = 0;
    int WarehouseCount = 1;
    bool EnableForeignKeys = true;
};

int ResolveObPartitionCount(const TObSchemaOptions& options);
std::string ObPartitioningStyle(const TObSchemaOptions& options);
std::string ForeignKeysModeLabel(bool enabled);
bool ParseForeignKeysMode(const std::string& value, bool& enabled);

} // namespace NTpcc

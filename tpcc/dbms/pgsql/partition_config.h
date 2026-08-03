#pragma once

#include <string>

namespace NTpcc {

inline constexpr const char* PG_PARTITIONING_NONE = "none";
inline constexpr const char* PG_PARTITIONING_WAREHOUSE_HASH = "warehouse_hash";

struct TPgPartitionConfig {
    // "none" or "warehouse_hash"
    std::string Partitioning = PG_PARTITIONING_NONE;
    // Hash modulus. 0 means derive from WarehouseCount when hashing.
    int PartitionCount = 0;
    // Used only to derive PartitionCount when hashing and count is 0.
    int WarehouseCount = 0;
};

bool IsPgPartitioningNone(const std::string& partitioning);
bool IsPgWarehouseHashPartitioning(const std::string& partitioning);

// Validates Partitioning string; throws std::runtime_error on unknown values.
void ValidatePgPartitioning(const std::string& partitioning);

// Derives a hash modulus from warehouse scale (design heuristic).
int DerivePgHashPartitionCount(int warehouseCount);

// Resolves PartitionCount for schema creation; throws if hashing cannot be configured.
int ResolvePgPartitionCount(const TPgPartitionConfig& config);

} // namespace NTpcc

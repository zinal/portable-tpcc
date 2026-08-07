#include "partition_config.h"

#include <algorithm>
#include <stdexcept>

namespace NTpcc {

namespace {

constexpr int MIN_HASH_PARTITIONS = 16;
constexpr int MAX_HASH_PARTITIONS = 1024;
constexpr int WAREHOUSES_PER_PARTITION_TARGET = 45;
constexpr int WAREHOUSES_PER_PARTITION_FLOOR = 100;

} // anonymous

bool IsPgPartitioningNone(const std::string& partitioning) {
    return partitioning.empty() || partitioning == PG_PARTITIONING_NONE;
}

bool IsPgWarehouseHashPartitioning(const std::string& partitioning) {
    return partitioning == PG_PARTITIONING_WAREHOUSE_HASH;
}

void ValidatePgPartitioning(const std::string& partitioning) {
    if (IsPgPartitioningNone(partitioning) || IsPgWarehouseHashPartitioning(partitioning)) {
        return;
    }
    throw std::runtime_error(
        "partitioning must be \"none\" or \"warehouse_hash\"");
}

int DerivePgHashPartitionCount(int warehouseCount) {
    if (warehouseCount <= 0) {
        throw std::runtime_error("warehouse count must be greater than zero to derive partition_count");
    }

    const int bySize =
        (warehouseCount + WAREHOUSES_PER_PARTITION_TARGET - 1) / WAREHOUSES_PER_PARTITION_TARGET;
    const int byFloor = std::max(
        MIN_HASH_PARTITIONS,
        (warehouseCount + WAREHOUSES_PER_PARTITION_FLOOR - 1) / WAREHOUSES_PER_PARTITION_FLOOR);
    const int derived = std::max(bySize, byFloor);
    return std::min(derived, MAX_HASH_PARTITIONS);
}

int ResolvePgPartitionCount(const TPgPartitionConfig& config) {
    ValidatePgPartitioning(config.Partitioning);

    if (IsPgPartitioningNone(config.Partitioning)) {
        if (config.PartitionCount != 0) {
            throw std::runtime_error(
                "partition_count is only valid when partitioning=warehouse_hash");
        }
        return 0;
    }

    if (config.PartitionCount < 0) {
        throw std::runtime_error("partition_count must not be negative");
    }
    if (config.PartitionCount > MAX_HASH_PARTITIONS) {
        throw std::runtime_error(
            "partition_count must not exceed " + std::to_string(MAX_HASH_PARTITIONS));
    }
    if (config.PartitionCount > 0) {
        return config.PartitionCount;
    }
    return DerivePgHashPartitionCount(config.WarehouseCount);
}

std::string ForeignKeysModeLabel(bool enabled) {
    return enabled ? "on" : "off";
}

bool ParseForeignKeysMode(const std::string& value, bool& enabled) {
    if (value == "on" || value == "true" || value == "1") {
        enabled = true;
        return true;
    }
    if (value == "off" || value == "false" || value == "0") {
        enabled = false;
        return true;
    }
    return false;
}

} // namespace NTpcc

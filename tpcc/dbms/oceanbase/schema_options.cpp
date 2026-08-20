#include "schema_options.h"

#include <algorithm>
#include <stdexcept>

namespace NTpcc {

namespace {

constexpr int MAX_OB_PARTITIONS = 8192;

} // namespace

int ResolveObPartitionCount(const TObSchemaOptions& options) {
    if (options.PartitionCount < -1) {
        throw std::runtime_error("partitions must be -1, 0, or a positive integer");
    }
    if (options.PartitionCount == -1) {
        return -1;
    }
    if (options.PartitionCount > MAX_OB_PARTITIONS) {
        throw std::runtime_error(
            "partitions must not exceed " + std::to_string(MAX_OB_PARTITIONS));
    }
    if (options.PartitionCount > 0) {
        return options.PartitionCount;
    }
    if (options.WarehouseCount <= 0) {
        throw std::runtime_error("warehouse count must be greater than zero to derive partitions");
    }
    return std::max(1, options.WarehouseCount);
}

int ResolveObIndexParallel(const TObSchemaOptions& options) {
    if (options.IndexParallel < 1) {
        throw std::runtime_error("index_parallel must be a positive integer");
    }
    return options.IndexParallel;
}

int ResolveObAnalyzeDegree(const TObSchemaOptions& options) {
    const int partitions = ResolveObPartitionCount(options);
    return partitions < 1 ? 1 : partitions;
}

std::string ObPartitioningStyle(const TObSchemaOptions& options) {
    return options.PartitionCount == -1 ? OB_PARTITIONING_NONE : OB_PARTITIONING_TABLEGROUP_HASH;
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

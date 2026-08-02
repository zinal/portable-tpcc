#pragma once

#include "warehouse_range.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace NTpcc {

struct TImportConfig {
    std::string ConnectionString;
    std::string Path;
    size_t WarehouseCount = 1;
    size_t LoadThreadCount = 0;
    bool UseTui = true;

    // Orchestrated loader: half-open warehouse ranges from run-config assignment.
    std::vector<TWarehouseRange> WarehouseRanges;
    bool OwnsGlobalData = true;
    int TotalWarehouses = 0;
    // Deterministic population seed (run-config data.seed). Default 1 for standalone CLI.
    uint64_t Seed = 1;
    // Current PG COPY loader streams one logical table/warehouse at a time; this
    // run-config value is reserved as an advisory row chunk hint for adapters
    // that support pre-serialized PutBatch rows.
    int BatchRows = 0;
    // Optional run identity for PutBatch logging / future checkpoints.
    std::string RunId;
};

struct TImportState {
    explicit TImportState(std::stop_token stopToken)
        : StopToken(stopToken)
    {}

    TImportState(const TImportState& other)
        : StopToken()
        , DataSizeLoaded(other.DataSizeLoaded.load(std::memory_order_relaxed))
        , WarehousesLoaded(other.WarehousesLoaded.load(std::memory_order_relaxed))
        , ApproximateDataSize(other.ApproximateDataSize)
    {}

    TImportState& operator=(const TImportState& other) {
        if (this != &other) {
            DataSizeLoaded.store(other.DataSizeLoaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
            WarehousesLoaded.store(other.WarehousesLoaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
            ApproximateDataSize = other.ApproximateDataSize;
        }
        return *this;
    }

    std::stop_token StopToken;
    std::atomic<size_t> DataSizeLoaded{0};
    std::atomic<size_t> WarehousesLoaded{0};
    size_t ApproximateDataSize = 0;
};

void ImportSync(const TImportConfig& config);

} // namespace NTpcc

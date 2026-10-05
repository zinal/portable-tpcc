#pragma once

#include "warehouse_range.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NTpcc {

struct TImportConfig {
    size_t WarehouseCount = 1;
    std::vector<TWarehouseRange> WarehouseRanges;
    bool OwnsGlobalData = true;
    int TotalWarehouses = 0;
    uint64_t Seed = 1;
    int BatchRows = 0;
    std::string RunId;
};

void ImportSync(const TImportConfig& config);

} // namespace NTpcc

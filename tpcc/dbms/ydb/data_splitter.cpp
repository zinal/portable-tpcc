#include "data_splitter.h"

#include <constants.h>

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace NTpcc {

namespace {

constexpr int DEFAULT_MIN_PARTITIONS = 50;
constexpr int DEFAULT_MIN_WAREHOUSES_PER_SHARD = 100;
constexpr int MIN_ITEMS_PER_SHARD = 100;
constexpr size_t DEFAULT_SHARD_SIZE_MB = 2048;

const std::unordered_map<std::string, double> PER_WAREHOUSE_MB = {
    {TABLE_STOCK, 45.0},
    {TABLE_CUSTOMER, 20.1},
    {TABLE_ORDER_LINE, 35.0},
    {TABLE_HISTORY, 2.4},
    {TABLE_OORDER, 1.5},
};

} // anonymous

TDataSplitter::TDataSplitter(int warehouseCount)
    : WarehouseCount_(warehouseCount)
{}

int TDataSplitter::CalcMinParts(int warehouseCount) {
    return std::max(DEFAULT_MIN_PARTITIONS, warehouseCount / DEFAULT_MIN_WAREHOUSES_PER_SHARD);
}

double TDataSplitter::GetPerWarehouseMB(const std::string& table) {
    auto it = PER_WAREHOUSE_MB.find(table);
    return it == PER_WAREHOUSE_MB.end() ? 0.0 : it->second;
}

std::vector<int> TDataSplitter::GetSplitKeys(const std::string& table) const {
    int minShardCount = CalcMinParts(WarehouseCount_);

    if (table == TABLE_ITEM) {
        int itemsPerShard = ITEM_COUNT / minShardCount;
        itemsPerShard = std::max(MIN_ITEMS_PER_SHARD, itemsPerShard);

        std::vector<int> splitKeys;
        for (int curItem = itemsPerShard; curItem < ITEM_COUNT; curItem += itemsPerShard) {
            splitKeys.push_back(curItem);
        }
        return splitKeys;
    }

    int warehousesPerShard = 0;
    auto it = PER_WAREHOUSE_MB.find(table);
    if (it != PER_WAREHOUSE_MB.end()) {
        double mbPerWh = it->second;
        warehousesPerShard = static_cast<int>((DEFAULT_SHARD_SIZE_MB + mbPerWh - 1) / mbPerWh);
        int byMinParts = (WarehouseCount_ + minShardCount - 1) / minShardCount;
        warehousesPerShard = std::min(warehousesPerShard, byMinParts);
    } else {
        warehousesPerShard = (WarehouseCount_ + minShardCount - 1) / minShardCount;
    }

    if (warehousesPerShard < 2) {
        return {};
    }

    std::vector<int> splitKeys;
    for (int split = 1 + warehousesPerShard; split < WarehouseCount_ + 1; split += warehousesPerShard) {
        splitKeys.push_back(split);
    }
    return splitKeys;
}

std::string TDataSplitter::GetSplitKeysString(const std::string& table) const {
    auto splitKeys = GetSplitKeys(table);
    std::ostringstream out;
    for (size_t i = 0; i < splitKeys.size(); ++i) {
        if (i) {
            out << ",";
        }
        out << splitKeys[i];
    }
    return out.str();
}

} // namespace NTpcc

#pragma once

#include <ops.h>

#include <vector>

namespace NTpcc {

struct TYdbStockBatchItem {
    int WarehouseID = 0;
    int ItemID = 0;
    int NewQuantity = 0;
    TMoney NewYtd;
    int NewOrderCount = 0;
    int NewRemoteCount = 0;
};

// Collapse per-line TUpdateStock ops (including duplicate item ids) into one
// row per (warehouse, item). The workflow already applied each line to the
// snapshot it read, so the last absolute values are the row to write.
std::vector<TYdbStockBatchItem> AggregateYdbStockUpdates(const std::vector<TSemanticOp>& ops);

template <typename T>
bool AllSemanticOpsAre(const std::vector<TSemanticOp>& ops) {
    if (ops.empty()) {
        return false;
    }
    for (const auto& op : ops) {
        if (!std::holds_alternative<T>(op)) {
            return false;
        }
    }
    return true;
}

} // namespace NTpcc

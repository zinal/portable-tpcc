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
// row per (warehouse, item). Last absolute quantity/ytd/order_cnt/remote_cnt
// wins, matching in-order application of the line ops.
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

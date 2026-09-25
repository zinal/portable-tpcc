#pragma once

#include <ops.h>

#include <vector>

namespace NTpcc {

struct TYdbStockBatchItem {
    int WarehouseID = 0;
    int ItemID = 0;
    int NewQuantity = 0;
    int OrderedQuantity = 0;
    int RemoteIncrement = 0;
    int LineCount = 0;
};

// Collapse per-line TUpdateStock ops (including duplicate item ids) into one
// row per (warehouse, item). Last absolute quantity wins; OrderedQuantity,
// RemoteIncrement, and LineCount are summed so the batched UPDATE matches
// in-order application of the single-row incremental statement.
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

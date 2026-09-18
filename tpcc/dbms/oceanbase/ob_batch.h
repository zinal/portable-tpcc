#pragma once

#include <constants.h>
#include <ops.h>

#include <cstddef>
#include <string>
#include <vector>

namespace NTpcc {

// New-Order has 5–15 lines (TPC-C §2.4.1.4). Unused-item rollback can drop that
// by one. Statement text is keyed only by this bounded count so the prepared-
// statement cache stays finite (efficiency plan Phase 2.1–2.3).
constexpr size_t ObBatchMaxRows = static_cast<size_t>(MAX_ITEMS);

struct TObStockBatchItem {
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
// in-order application of the current single-row incremental statement.
std::vector<TObStockBatchItem> AggregateObStockUpdates(const std::vector<TSemanticOp>& ops);

std::vector<int> UniqueItemIds(const std::vector<int>& ids);
std::vector<TStockKey> UniqueSortedStockKeys(const std::vector<TStockKey>& keys);

std::string BuildObGetItemsSql(size_t n);
std::string BuildObGetStocksForUpdateSql(size_t n);
std::string BuildObStockUpdateBatchSql(size_t n);
std::string BuildObOrderLineInsertSql(size_t n);

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

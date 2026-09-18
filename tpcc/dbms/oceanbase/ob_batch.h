#pragma once

#include <constants.h>
#include <ops.h>

#include <cstddef>
#include <string>
#include <string_view>
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

struct TObDeliveryOrderKey {
    int DistrictID = 0;
    int OrderID = 0;
};

// Phase 3: Payment location (JOIN UPDATE + JOIN SELECT) in one multi-statement.
std::string BuildObPaymentLocationSql(int warehouseId, int districtId, std::string_view amount);

// Independent oldest new_order lock per district, district order, no SKIP LOCKED.
std::string BuildObOldestNewOrdersSql(int warehouseId);

std::string BuildObDeliveryOrderInfoSql(
    int warehouseId,
    const std::vector<TObDeliveryOrderKey>& orders);

std::string BuildObDeliveryCompleteSql(
    int warehouseId,
    int carrierId,
    const std::vector<TObDeliveryOrderKey>& orders);

std::string BuildObDeliveryApplySql(size_t n);

// Last Delivery customer apply. COMMIT is not included: the adapter checks
// affected-row cardinality, then COMMIT or ROLLBACK.
std::string BuildObDeliveryFinishSql(const TApplyDeliveryToCustomer& apply);

// Deferred Payment customer update + history insert. COMMIT is not included:
// the adapter checks affected-row cardinality, then COMMIT or ROLLBACK.
std::string BuildObPaymentFinishSql(
    const TUpdateCustomerPayment& update,
    const TInsertPaymentHistory& history,
    const std::string& quotedCustomerData,
    const std::string& quotedHistoryData);

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

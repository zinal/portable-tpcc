#pragma once

#include "rows.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace NTpcc::NGenerator {

// Fixed epoch used so load timestamps are fully determined by run-config seed.
// 2020-01-01T00:00:00Z
constexpr int64_t LoadEpochUnix = 1577836800LL;

int64_t LoadTimestampUnix(uint64_t seed, uint64_t salt);

TItemRow GenerateItem(uint64_t seed, int itemId);
TWarehouseRow GenerateWarehouse(uint64_t seed, int warehouseId);
TDistrictRow GenerateDistrict(uint64_t seed, int warehouseId, int districtId);
TStockRow GenerateStock(uint64_t seed, int warehouseId, int itemId);
TCustomerRow GenerateCustomer(uint64_t seed, int warehouseId, int districtId, int customerId);
THistoryRow GenerateHistory(uint64_t seed, int warehouseId, int districtId, int customerId);

// Order id in [1, CUSTOMERS_PER_DISTRICT]; customer permutation is deterministic.
TOrderRow GenerateOrder(uint64_t seed, int warehouseId, int districtId, int orderId, int customerId);
TNewOrderRow GenerateNewOrder(int warehouseId, int districtId, int orderId);
// When deliveryUnix is set (initial delivered orders), every line gets
// OL_DELIVERY_D = *deliveryUnix, matching O_ENTRY_D per TPC-C §4.3.3.1.
std::vector<TOrderLineRow> GenerateOrderLines(
    uint64_t seed, int warehouseId, int districtId, int orderId, int olCnt,
    std::optional<int64_t> deliveryUnix);

// Deterministic order-line count in [MIN_ITEMS, MAX_ITEMS] for a customer order.
int GenerateOrderLineCount(uint64_t seed, int warehouseId, int districtId, int customerId);

// Customer id permutation for initial orders (index 0 .. CUSTOMERS_PER_DISTRICT-1).
std::vector<int> InitialOrderCustomerPermutation(uint64_t seed, int warehouseId, int districtId);

} // namespace NTpcc::NGenerator

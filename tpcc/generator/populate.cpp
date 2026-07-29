#include "populate.h"

#include "seed.h"
#include "strings.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace NTpcc::NGenerator {

namespace {

TRate RandomTax(TSeededRng& rng) {
    // TPC-C: uniform [0.0000 .. 0.2000]
    return TRate::FromUnits(static_cast<int64_t>(RandomNumber(rng, 0, 2000)));
}

TRate RandomDiscount(TSeededRng& rng) {
    // TPC-C: uniform [0.0000 .. 0.5000]
    return TRate::FromUnits(static_cast<int64_t>(RandomNumber(rng, 0, 5000)));
}

} // namespace

int64_t LoadTimestampUnix(uint64_t seed, uint64_t salt) {
    TSeededRng rng = RootRng(seed).Fork(salt ^ 0x54494D45ULL);
    // Spread a few days past the fixed epoch without using wall clock.
    return LoadEpochUnix + static_cast<int64_t>(RandomNumber(rng, 0, 7ULL * 24 * 3600));
}

TItemRow GenerateItem(uint64_t seed, int itemId) {
    TSeededRng rng = TableRng(seed, SaltItem).Fork(static_cast<uint64_t>(itemId));
    TItemRow row;
    row.Id = itemId;
    row.Name = RandomAString(rng, 14, 24);
    row.Price = TMoney::FromCents(static_cast<int64_t>(RandomNumber(rng, 100, 10000)));
    row.Data = RandomDataWithOriginalChance(rng, 26, 50, 10);
    row.ImageId = static_cast<int>(RandomNumber(rng, 1, 10000));
    return row;
}

TWarehouseRow GenerateWarehouse(uint64_t seed, int warehouseId) {
    TSeededRng rng = WarehouseRng(seed, SaltWarehouse, warehouseId);
    TWarehouseRow row;
    row.Id = warehouseId;
    row.Ytd = WAREHOUSE_INITIAL_YTD;
    row.Tax = RandomTax(rng);
    row.Name = RandomAString(rng, 6, 10);
    row.Street1 = RandomAString(rng, 10, 20);
    row.Street2 = RandomAString(rng, 10, 20);
    row.City = RandomAString(rng, 10, 20);
    row.State = RandomAString(rng, 2, 2, 'A');
    row.Zip = RandomNumericString(rng, 4) + "11111";
    return row;
}

TDistrictRow GenerateDistrict(uint64_t seed, int warehouseId, int districtId) {
    TSeededRng rng = DistrictRng(seed, SaltDistrict, warehouseId, districtId);
    TDistrictRow row;
    row.WarehouseId = warehouseId;
    row.Id = districtId;
    row.Ytd = DISTRICT_INITIAL_YTD;
    row.Tax = RandomTax(rng);
    row.NextOrderId = CUSTOMERS_PER_DISTRICT + 1;
    row.Name = RandomAString(rng, 6, 10);
    row.Street1 = RandomAString(rng, 10, 20);
    row.Street2 = RandomAString(rng, 10, 20);
    row.City = RandomAString(rng, 10, 20);
    row.State = RandomAString(rng, 2, 2, 'A');
    row.Zip = RandomNumericString(rng, 4) + "11111";
    return row;
}

TStockRow GenerateStock(uint64_t seed, int warehouseId, int itemId) {
    TSeededRng rng = WarehouseRng(seed, SaltStock, warehouseId).Fork(static_cast<uint64_t>(itemId));
    TStockRow row;
    row.WarehouseId = warehouseId;
    row.ItemId = itemId;
    row.Quantity = static_cast<int>(RandomNumber(rng, 10, 100));
    row.Ytd = TMoney::FromCents(0);
    row.OrderCount = 0;
    row.RemoteCount = 0;
    row.Data = RandomDataWithOriginalChance(rng, 26, 50, 10);
    for (size_t i = 0; i < row.Dist.size(); ++i) {
        row.Dist[i] = RandomAString(rng, 24);
    }
    return row;
}

TCustomerRow GenerateCustomer(uint64_t seed, int warehouseId, int districtId, int customerId) {
    TSeededRng rng = DistrictRng(seed, SaltCustomer, warehouseId, districtId)
                         .Fork(static_cast<uint64_t>(customerId));
    TCustomerRow row;
    row.WarehouseId = warehouseId;
    row.DistrictId = districtId;
    row.Id = customerId;
    row.First = RandomAString(rng, 8, 16);
    row.Middle = "OE";
    if (customerId <= 1000) {
        row.Last = GetLastName(customerId - 1);
    } else {
        row.Last = GetNonUniformRandomLastNameForLoad(rng);
    }
    row.Street1 = RandomAString(rng, 10, 20);
    row.Street2 = RandomAString(rng, 10, 20);
    row.City = RandomAString(rng, 10, 20);
    row.State = RandomAString(rng, 2, 2, 'A');
    row.Zip = RandomNumericString(rng, 4) + "11111";
    row.Phone = RandomNumericString(rng, 16);
    row.SinceUnix = LoadTimestampUnix(seed, SaltCustomer ^ static_cast<uint64_t>(warehouseId) << 32
                                                     ^ static_cast<uint64_t>(districtId) << 16
                                                     ^ static_cast<uint64_t>(customerId));
    row.Credit = RandomNumber(rng, 1, 100) <= 10 ? "BC" : "GC";
    row.CreditLimit = CUSTOMER_INITIAL_CREDIT_LIMIT;
    row.Discount = RandomDiscount(rng);
    row.Balance = CUSTOMER_INITIAL_BALANCE;
    row.YtdPayment = CUSTOMER_INITIAL_YTD_PAYMENT;
    row.PaymentCount = 1;
    row.DeliveryCount = 0;
    row.Data = RandomAString(rng, 300, 500);
    return row;
}

THistoryRow GenerateHistory(uint64_t seed, int warehouseId, int districtId, int customerId) {
    TSeededRng rng = DistrictRng(seed, SaltHistory, warehouseId, districtId)
                         .Fork(static_cast<uint64_t>(customerId));
    THistoryRow row;
    row.CustomerId = customerId;
    row.CustomerDistrictId = districtId;
    row.CustomerWarehouseId = warehouseId;
    row.DistrictId = districtId;
    row.WarehouseId = warehouseId;
    row.DateUnix = LoadTimestampUnix(seed, SaltHistory ^ static_cast<uint64_t>(warehouseId) << 32
                                                   ^ static_cast<uint64_t>(districtId) << 16
                                                   ^ static_cast<uint64_t>(customerId));
    row.Amount = CUSTOMER_INITIAL_YTD_PAYMENT;
    row.Data = RandomAString(rng, 12, 24);
    return row;
}

int GenerateOrderLineCount(uint64_t seed, int warehouseId, int districtId, int customerId) {
    TSeededRng rng = DistrictRng(seed, SaltOrderLine, warehouseId, districtId)
                         .Fork(static_cast<uint64_t>(customerId));
    return static_cast<int>(RandomNumber(rng, MIN_ITEMS, MAX_ITEMS));
}

std::vector<int> InitialOrderCustomerPermutation(uint64_t seed, int warehouseId, int districtId) {
    std::vector<int> ids(CUSTOMERS_PER_DISTRICT);
    std::iota(ids.begin(), ids.end(), 1);
    TSeededRng rng = DistrictRng(seed, SaltOrder, warehouseId, districtId);
    for (int i = CUSTOMERS_PER_DISTRICT - 1; i > 0; --i) {
        int j = static_cast<int>(RandomNumber(rng, 0, i));
        std::swap(ids[i], ids[j]);
    }
    return ids;
}

TOrderRow GenerateOrder(uint64_t seed, int warehouseId, int districtId, int orderId, int customerId) {
    TSeededRng rng = DistrictRng(seed, SaltOrder, warehouseId, districtId)
                         .Fork(static_cast<uint64_t>(orderId));
    TOrderRow row;
    row.WarehouseId = warehouseId;
    row.DistrictId = districtId;
    row.Id = orderId;
    row.CustomerId = customerId;
    row.OlCnt = GenerateOrderLineCount(seed, warehouseId, districtId, customerId);
    row.AllLocal = 1;
    row.EntryUnix = LoadTimestampUnix(seed, SaltOrder ^ static_cast<uint64_t>(warehouseId) << 32
                                                 ^ static_cast<uint64_t>(districtId) << 16
                                                 ^ static_cast<uint64_t>(orderId));
    if (orderId < FIRST_UNPROCESSED_O_ID) {
        row.CarrierId = static_cast<int>(RandomNumber(rng, 1, 10));
    }
    return row;
}

TNewOrderRow GenerateNewOrder(int warehouseId, int districtId, int orderId) {
    TNewOrderRow row;
    row.WarehouseId = warehouseId;
    row.DistrictId = districtId;
    row.OrderId = orderId;
    return row;
}

std::vector<TOrderLineRow> GenerateOrderLines(
    uint64_t seed, int warehouseId, int districtId, int orderId, int olCnt, bool delivered)
{
    TSeededRng rng = DistrictRng(seed, SaltOrderLine, warehouseId, districtId)
                         .Fork(static_cast<uint64_t>(orderId));
    std::vector<TOrderLineRow> lines;
    lines.reserve(static_cast<size_t>(olCnt));
    for (int ol = 1; ol <= olCnt; ++ol) {
        TOrderLineRow line;
        line.WarehouseId = warehouseId;
        line.DistrictId = districtId;
        line.OrderId = orderId;
        line.Number = ol;
        line.ItemId = static_cast<int>(RandomNumber(rng, 1, ITEM_COUNT));
        line.SupplyWarehouseId = warehouseId;
        line.Quantity = 5;
        if (delivered) {
            line.Amount = TMoney::FromCents(0);
            line.DeliveryUnix = LoadTimestampUnix(seed, SaltOrderLine ^ static_cast<uint64_t>(orderId) << 16
                                                               ^ static_cast<uint64_t>(ol));
        } else {
            line.Amount = TMoney::FromCents(static_cast<int64_t>(RandomNumber(rng, 1, 999999)));
        }
        line.DistInfo = RandomAString(rng, 24);
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace NTpcc::NGenerator

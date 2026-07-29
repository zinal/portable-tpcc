#pragma once

#include <constants.h>
#include <money.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace NTpcc::NGenerator {

struct TItemRow {
    int Id = 0;
    std::string Name;
    TMoney Price;
    std::string Data;
    int ImageId = 0;
};

struct TWarehouseRow {
    int Id = 0;
    TMoney Ytd;
    TRate Tax;
    std::string Name;
    std::string Street1;
    std::string Street2;
    std::string City;
    std::string State;
    std::string Zip;
};

struct TDistrictRow {
    int WarehouseId = 0;
    int Id = 0;
    TMoney Ytd;
    TRate Tax;
    int NextOrderId = 0;
    std::string Name;
    std::string Street1;
    std::string Street2;
    std::string City;
    std::string State;
    std::string Zip;
};

struct TStockRow {
    int WarehouseId = 0;
    int ItemId = 0;
    int Quantity = 0;
    TMoney Ytd;
    int OrderCount = 0;
    int RemoteCount = 0;
    std::string Data;
    std::array<std::string, DISTRICT_COUNT> Dist;
};

struct TCustomerRow {
    int WarehouseId = 0;
    int DistrictId = 0;
    int Id = 0;
    std::string First;
    std::string Middle;
    std::string Last;
    std::string Street1;
    std::string Street2;
    std::string City;
    std::string State;
    std::string Zip;
    std::string Phone;
    // Deterministic load timestamp as seconds since Unix epoch (UTC).
    int64_t SinceUnix = 0;
    std::string Credit;
    TMoney CreditLimit;
    TRate Discount;
    TMoney Balance;
    TMoney YtdPayment;
    int PaymentCount = 0;
    int DeliveryCount = 0;
    std::string Data;
};

struct THistoryRow {
    int CustomerId = 0;
    int CustomerDistrictId = 0;
    int CustomerWarehouseId = 0;
    int DistrictId = 0;
    int WarehouseId = 0;
    int64_t DateUnix = 0;
    TMoney Amount;
    std::string Data;
};

struct TOrderRow {
    int WarehouseId = 0;
    int DistrictId = 0;
    int Id = 0;
    int CustomerId = 0;
    std::optional<int> CarrierId;
    int OlCnt = 0;
    int AllLocal = 1;
    int64_t EntryUnix = 0;
};

struct TNewOrderRow {
    int WarehouseId = 0;
    int DistrictId = 0;
    int OrderId = 0;
};

struct TOrderLineRow {
    int WarehouseId = 0;
    int DistrictId = 0;
    int OrderId = 0;
    int Number = 0;
    int ItemId = 0;
    int SupplyWarehouseId = 0;
    std::optional<int64_t> DeliveryUnix;
    int Quantity = 0;
    TMoney Amount;
    std::string DistInfo;
};

} // namespace NTpcc::NGenerator

#pragma once

#include "money.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace NTpcc {

// ---------------------------------------------------------------------------
// Shared row / payload types returned inside TOperationResult::Payload
// ---------------------------------------------------------------------------

struct TCustomerRow {
    int CustomerID = 0;
    std::string First;
    std::string Middle;
    std::string Last;
    std::string Street1;
    std::string Street2;
    std::string City;
    std::string State;
    std::string Zip;
    std::string Phone;
    std::string Since;
    std::string Credit;
    TMoney CreditLimit;
    TRate Discount;
    TMoney Balance;
    TMoney YtdPayment;
    int PaymentCount = 0;
    int DeliveryCount = 0;
    std::string Data;
};

struct TItemRow {
    int ItemID = 0;
    TMoney Price;
    std::string Name;
    std::string Data;
};

struct TStockKey {
    int WarehouseID = 0;
    int ItemID = 0;
};

struct TStockRow {
    int WarehouseID = 0;
    int ItemID = 0;
    int Quantity = 0;
    TMoney Ytd;
    int OrderCount = 0;
    int RemoteCount = 0;
    std::string Data;
    std::string DistInfo;
};

struct TOrderHeader {
    int OrderID = 0;
    int CustomerID = 0;
    std::optional<int> CarrierID;
    std::string EntryDate;
};

struct TOrderStatusLine {
    int ItemID = 0;
    int SupplyWarehouseID = 0;
    int Quantity = 0;
    TMoney Amount;
    std::string DeliveryDate;
};

struct TWarehouseDistrictInfo {
    std::string WarehouseName;
    std::string WarehouseStreet1;
    std::string WarehouseStreet2;
    std::string WarehouseCity;
    std::string WarehouseState;
    std::string WarehouseZip;
    std::string DistrictName;
    std::string DistrictStreet1;
    std::string DistrictStreet2;
    std::string DistrictCity;
    std::string DistrictState;
    std::string DistrictZip;
};

struct TDeliveryOrderInfo {
    int CustomerID = 0;
    TMoney TotalAmount;
    int LineCount = 0;
};

struct TDistrictOrderReservation {
    int NextOrderID = 0;
    TRate DistrictTax;
};

// ---------------------------------------------------------------------------
// Semantic operations (closed set)
// ---------------------------------------------------------------------------

struct TGetCustomerById {
    int WarehouseID = 0;
    int DistrictID = 0;
    int CustomerID = 0;
};

struct TGetCustomersByLastName {
    int WarehouseID = 0;
    int DistrictID = 0;
    std::string LastName;
};

struct TGetWarehouseTax {
    int WarehouseID = 0;
};

struct TReserveDistrictOrderId {
    int WarehouseID = 0;
    int DistrictID = 0;
};

struct TCreateOrder {
    int WarehouseID = 0;
    int DistrictID = 0;
    int OrderID = 0;
    int CustomerID = 0;
    int LineCount = 0;
    int AllLocal = 1;
};

struct TGetItems {
    std::vector<int> ItemIDs;
};

struct TGetStocksForUpdate {
    int DistrictID = 0;
    std::vector<TStockKey> Stocks;
};

struct TUpdateStock {
    int WarehouseID = 0;
    int ItemID = 0;
    int NewQuantity = 0;
    int OrderedQuantity = 0;
    int RemoteIncrement = 0;
};

struct TInsertOrderLine {
    int WarehouseID = 0;
    int DistrictID = 0;
    int OrderID = 0;
    int LineNumber = 0;
    int ItemID = 0;
    int SupplyWarehouseID = 0;
    int Quantity = 0;
    TMoney Amount;
    std::string DistInfo;
};

struct TApplyPaymentToLocation {
    int WarehouseID = 0;
    int DistrictID = 0;
    TMoney Amount;
};

struct TGetCustomerData {
    int WarehouseID = 0;
    int DistrictID = 0;
    int CustomerID = 0;
};

struct TUpdateCustomerPayment {
    int WarehouseID = 0;
    int DistrictID = 0;
    int CustomerID = 0;
    TMoney NewBalance;
    TMoney NewYtdPayment;
    int NewPaymentCount = 0;
    bool UpdateData = false;
    std::string NewData;
};

struct TInsertPaymentHistory {
    int CustomerWarehouseID = 0;
    int CustomerDistrictID = 0;
    int CustomerID = 0;
    int PaymentWarehouseID = 0;
    int PaymentDistrictID = 0;
    TMoney Amount;
    std::string Data;
};

struct TGetLatestCustomerOrder {
    int WarehouseID = 0;
    int DistrictID = 0;
    int CustomerID = 0;
};

struct TGetOrderStatusLines {
    int WarehouseID = 0;
    int DistrictID = 0;
    int OrderID = 0;
};

struct TGetOldestNewOrder {
    int WarehouseID = 0;
    int DistrictID = 0;
};

struct TGetDeliveryOrderInfo {
    int WarehouseID = 0;
    int DistrictID = 0;
    int OrderID = 0;
};

struct TCompleteOrderDelivery {
    int WarehouseID = 0;
    int DistrictID = 0;
    int OrderID = 0;
    int CarrierID = 0;
};

struct TApplyDeliveryToCustomer {
    int WarehouseID = 0;
    int DistrictID = 0;
    int CustomerID = 0;
    TMoney Amount;
};

struct TCountRecentLowStock {
    int WarehouseID = 0;
    int DistrictID = 0;
    int Threshold = 0;
    int RecentOrderCount = 20;
};

using TSemanticOp = std::variant<
    TGetCustomerById,
    TGetCustomersByLastName,
    TGetWarehouseTax,
    TReserveDistrictOrderId,
    TCreateOrder,
    TGetItems,
    TGetStocksForUpdate,
    TUpdateStock,
    TInsertOrderLine,
    TApplyPaymentToLocation,
    TGetCustomerData,
    TUpdateCustomerPayment,
    TInsertPaymentHistory,
    TGetLatestCustomerOrder,
    TGetOrderStatusLines,
    TGetOldestNewOrder,
    TGetDeliveryOrderInfo,
    TCompleteOrderDelivery,
    TApplyDeliveryToCustomer,
    TCountRecentLowStock>;

using TOperationPayload = std::variant<
    std::monostate,
    TCustomerRow,
    std::vector<TCustomerRow>,
    TMoney,
    TRate,
    TDistrictOrderReservation,
    std::vector<TItemRow>,
    std::vector<TStockRow>,
    TWarehouseDistrictInfo,
    std::string,
    TOrderHeader,
    std::vector<TOrderStatusLine>,
    int,
    TDeliveryOrderInfo>;

} // namespace NTpcc

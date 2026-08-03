#pragma once

#include <cstdint>
#include <string_view>

namespace NTpcc {

enum class EObQueryId : uint16_t {
    GetWarehouseTax,
    ReserveDistrictOrderId,
    UpdateDistrictNextOrderId,
    GetCustomerById,
    GetItems,
    CreateOrder,
    CreateNewOrder,
    UpdateStock,
    InsertOrderLine,
    CountRecentDistrict,
    CountRecentLowStock,
    GetOldestNewOrder,
    ApplyPaymentWarehouse,
    ApplyPaymentDistrict,
    SelectPaymentWarehouse,
    SelectPaymentDistrict,
    InsertPaymentHistory,
    GetCustomersByLastName,
    GetStockForUpdate,
    GetCustomerData,
    UpdateCustomerPaymentWithData,
    UpdateCustomerPayment,
    GetLatestCustomerOrder,
    GetOrderStatusLines,
    GetDeliveryOrderCustomer,
    GetDeliveryOrderLines,
    DeleteNewOrder,
    UpdateOrderCarrier,
    UpdateOrderLineDelivery,
    ApplyDeliveryToCustomer,
    SimulationSelectCastInt,
    Count
};

std::string_view QuerySql(EObQueryId id);
bool QueryIsSelect(EObQueryId id);

} // namespace NTpcc

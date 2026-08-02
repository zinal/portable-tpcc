#include "workflows.h"
#include "workflow_util.h"

#include <constants.h>
#include <log.h>
#include <money.h>
#include <rng.h>

#include <array>
#include <optional>

namespace NTpcc {

namespace {

struct TOrderData {
    int OrderID = 0;
    int CustomerId = 0;
    TMoney TotalAmount;
    int LineCount = 0;
};

} // namespace

TFuture<bool> GetDeliveryTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    ITpccTransaction& tx)
{
    auto startTs = std::chrono::steady_clock::now();

    TTransactionInflightGuard guard;
    co_await TTaskReady(context.TaskQueue, context.TerminalID);

    struct TInputs {
        int WarehouseID;
        int CarrierID;
    };

    const auto& in = FixedTransactionInputs<TInputs>(context, [&] {
        return TInputs{
            .WarehouseID = static_cast<int>(context.WarehouseID),
            .CarrierID = static_cast<int>(RandomNumber(1, 10)),
        };
    });

    LOG_T("Terminal " << context.TerminalID << " started Delivery: W=" << in.WarehouseID);

    std::array<std::optional<TOrderData>, DISTRICT_COUNT> orders;

    for (int districtID = DISTRICT_LOW_ID; districtID <= DISTRICT_HIGH_ID; ++districtID) {
        int orderID = 0;
        {
            auto r = co_await SuspendExecute(tx, context, TGetOldestNewOrder{
                in.WarehouseID, districtID});
            ThrowIfRetryable(r);
            if (!r.Ok) {
                co_return FailPermanent(context.TerminalID, "Delivery oldest new order failed");
            }
            if (r.ActualRows == 0) {
                LOG_T("Terminal " << context.TerminalID << " no new orders for district " << districtID);
                continue;
            }
            orderID = std::get<int>(r.Payload);
        }

        auto& order = orders[districtID - DISTRICT_LOW_ID].emplace();
        order.OrderID = orderID;

        {
            auto r = co_await SuspendExecute(tx, context, TGetDeliveryOrderInfo{
                in.WarehouseID, districtID, orderID});
            ThrowIfRetryable(r);
            if (!r.Ok) {
                co_return FailPermanent(context.TerminalID, "Delivery order info failed");
            }
            const auto& info = std::get<TDeliveryOrderInfo>(r.Payload);
            if (info.LineCount == 0) {
                co_return FailPermanent(context.TerminalID, "Delivery no order lines");
            }
            order.CustomerId = info.CustomerID;
            order.TotalAmount = info.TotalAmount;
            order.LineCount = info.LineCount;
        }
    }

    for (int districtID = DISTRICT_LOW_ID; districtID <= DISTRICT_HIGH_ID; ++districtID) {
        if (!orders[districtID - DISTRICT_LOW_ID]) {
            continue;
        }
        auto& order = *orders[districtID - DISTRICT_LOW_ID];

        {
            auto r = co_await SuspendExecute(tx, context, TCompleteOrderDelivery{
                in.WarehouseID, districtID, order.OrderID, in.CarrierID, order.LineCount});
            ThrowIfRetryable(r);
            if (!r.Ok) {
                co_return FailPermanent(context.TerminalID, "Delivery complete order failed");
            }
        }

        {
            auto r = co_await SuspendExecute(tx, context, TApplyDeliveryToCustomer{
                in.WarehouseID, districtID, order.CustomerId, order.TotalAmount});
            ThrowIfRetryable(r);
            if (!r.Ok) {
                co_return FailPermanent(context.TerminalID, "Delivery apply customer failed");
            }
        }
    }

    LOG_T("Terminal " << context.TerminalID << " committing Delivery");
    auto commit = co_await SuspendCommit(tx, context);
    ThrowIfCommitFailed(commit);

    latency = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startTs);
    co_return true;
}

} // namespace NTpcc

#include "workflows.h"
#include "workflow_util.h"

#include <constants.h>
#include <log.h>
#include <rng.h>

namespace NTpcc {

TFuture<bool> GetOrderStatusTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    ITpccTransaction& tx)
{
    auto startTs = std::chrono::steady_clock::now();

    TTransactionInflightGuard guard;
    co_await TTaskReady(context.TaskQueue, context.TerminalID);

    struct TInputs {
        int WarehouseID;
        int DistrictID;
        bool LookupByName;
        std::string LastName;
        int CustomerID;
    };

    const auto& in = FixedTransactionInputs<TInputs>(context, [&] {
        TInputs generated;
        generated.WarehouseID = static_cast<int>(context.WarehouseID);
        generated.DistrictID = RandomNumber(DISTRICT_LOW_ID, DISTRICT_HIGH_ID);
        generated.LookupByName = RandomNumber(1, 100) <= 60;
        if (generated.LookupByName) {
            generated.LastName = GetNonUniformRandomLastNameForRun();
            generated.CustomerID = 0;
        } else {
            generated.CustomerID = GetRandomCustomerID();
        }
        return generated;
    });

    LOG_T("Terminal " << context.TerminalID << " started OrderStatus: W=" << in.WarehouseID << ", D=" << in.DistrictID);

    TCustomerRow customer;
    if (in.LookupByName) {
        auto r = co_await SuspendExecute(tx, context, TGetCustomersByLastName{
            in.WarehouseID, in.DistrictID, in.LastName});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "OrderStatus customer by name failed");
        }
        auto selected = SelectCustomerByLastNameMedian(
            std::get<std::vector<TCustomerRow>>(r.Payload));
        if (!selected) {
            co_return FailPermanent(context.TerminalID, "OrderStatus no customer by name");
        }
        customer = std::move(*selected);
    } else {
        auto r = co_await SuspendExecute(tx, context, TGetCustomerById{
            in.WarehouseID, in.DistrictID, in.CustomerID});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "OrderStatus customer not found");
        }
        customer = std::get<TCustomerRow>(r.Payload);
    }

    int orderID = 0;
    {
        auto r = co_await SuspendExecute(tx, context, TGetLatestCustomerOrder{
            in.WarehouseID, in.DistrictID, customer.CustomerID});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "OrderStatus latest order failed");
        }
        if (r.ActualRows == 0) {
            LOG_T("Terminal " << context.TerminalID << " customer has no orders");
            auto commit = co_await SuspendCommit(tx, context);
            ThrowIfCommitFailed(commit);
            latency = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - startTs);
            co_return true;
        }
        orderID = std::get<TOrderHeader>(r.Payload).OrderID;
    }

    LOG_T("Terminal " << context.TerminalID << " committing OrderStatus: C=" << customer.CustomerID << ", O=" << orderID);
    {
        auto finalResult = co_await SuspendExecuteFinalAndCommit(tx, context, TGetOrderStatusLines{
            in.WarehouseID, in.DistrictID, orderID});
        ThrowIfRetryable(finalResult.Operation);
        if (!finalResult.Operation.Ok) {
            co_return FailPermanent(context.TerminalID, "OrderStatus lines failed");
        }
        ThrowIfCommitFailed(finalResult.Commit);
    }

    latency = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startTs);
    co_return true;
}

} // namespace NTpcc

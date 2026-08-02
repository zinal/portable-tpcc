#include "workflows.h"
#include "workflow_util.h"

#include <constants.h>
#include <log.h>
#include <rng.h>

namespace NTpcc {

TFuture<bool> GetStockLevelTask(
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
        int Threshold;
    };

    const auto& in = FixedTransactionInputs<TInputs>(context, [&] {
        return TInputs{
            .WarehouseID = static_cast<int>(context.WarehouseID),
            // TPC-C §2.8.1.1: D_ID is the terminal's home district, constant
            // for the whole measurement interval.
            .DistrictID = static_cast<int>(context.DistrictID),
            .Threshold = static_cast<int>(RandomNumber(10, 20)),
        };
    });

    LOG_T("Terminal " << context.TerminalID << " started StockLevel: W=" << in.WarehouseID << ", D=" << in.DistrictID);

    {
        auto r = co_await SuspendExecute(tx, context, TCountRecentLowStock{
            in.WarehouseID, in.DistrictID, in.Threshold, 20});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "StockLevel count failed");
        }
    }

    LOG_T("Terminal " << context.TerminalID << " committing StockLevel");
    auto commit = co_await SuspendCommit(tx, context);
    ThrowIfCommitFailed(commit);

    latency = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startTs);
    co_return true;
}

} // namespace NTpcc

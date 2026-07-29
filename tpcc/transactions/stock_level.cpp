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
            .DistrictID = static_cast<int>(RandomNumber(DISTRICT_LOW_ID, DISTRICT_HIGH_ID)),
            .Threshold = static_cast<int>(RandomNumber(10, 20)),
        };
    });

    LOG_T("Terminal {} started StockLevel: W={}, D={}",
          context.TerminalID, in.WarehouseID, in.DistrictID);

    {
        auto r = co_await SuspendExecute(tx, context, TCountRecentLowStock{
            in.WarehouseID, in.DistrictID, in.Threshold, 20});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "StockLevel count failed");
        }
    }

    LOG_T("Terminal {} committing StockLevel", context.TerminalID);
    auto commit = co_await SuspendCommit(tx, context);
    ThrowIfCommitFailed(commit);

    latency = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startTs);
    co_return true;
}

} // namespace NTpcc

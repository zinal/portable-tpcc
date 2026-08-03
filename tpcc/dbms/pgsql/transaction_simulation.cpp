#include "transactions.h"
#include "tpcc_session.h"

#include <rng.h>
#include <coro_traits.h>

#include <constants.h>
#include <log.h>

namespace NTpcc {

TFuture<bool> GetSimulationTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    TPgTpccTransaction& tx)
{
    auto startTs = std::chrono::steady_clock::now();

    TTransactionInflightGuard guard;
    co_await TTaskReady(context.TaskQueue, context.TerminalID);

    LOG_T("Terminal " << context.TerminalID << " started simulated transaction");

    for (size_t i = 0; i < 10; ++i) {
        RandomNumber(DISTRICT_LOW_ID, DISTRICT_HIGH_ID);
    }

    for (int i = 0; i < context.SimulateTransactionSelect1; ++i) {
        auto result = co_await TSuspendWithFuture(
            tx.ExecuteSelect1(),
            context.TaskQueue, context.TerminalID);
        if (!result.Ok) {
            co_return false;
        }
        LOG_T("Terminal " << context.TerminalID << " select1 iteration " << i);
    }

    auto commit = co_await TSuspendWithFuture(tx.Commit(), context.TaskQueue, context.TerminalID);
    if (commit.Outcome != ECommitOutcome::Committed) {
        co_return false;
    }

    auto endTs = std::chrono::steady_clock::now();
    latency = std::chrono::duration_cast<std::chrono::microseconds>(endTs - startTs);

    co_return true;
}

} // namespace NTpcc

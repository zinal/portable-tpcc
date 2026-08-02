#include "transactions.h"
#include "ydb_session.h"

#include <constants.h>
#include <coro_traits.h>
#include <log.h>
#include <rng.h>

namespace NTpcc {

TFuture<bool> GetSimulationTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    TYdbTpccTransaction& tx)
{
    auto startTs = std::chrono::steady_clock::now();

    TTransactionInflightGuard guard;
    co_await TTaskReady(context.TaskQueue, context.TerminalID);

    for (size_t i = 0; i < 10; ++i) {
        RandomNumber(DISTRICT_LOW_ID, DISTRICT_HIGH_ID);
    }

    if (context.SimulateTransactionMs != 0) {
        co_await TSuspend(
            context.TaskQueue,
            context.TerminalID,
            std::chrono::milliseconds(context.SimulateTransactionMs));
        latency = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startTs);
        co_return true;
    }

    for (int i = 0; i < context.SimulateTransactionSelect1; ++i) {
        auto result = co_await TSuspendWithFuture(
            tx.ExecuteSelect1(),
            context.TaskQueue,
            context.TerminalID);
        if (!result.Ok) {
            co_return false;
        }
        LOG_T("Terminal " << context.TerminalID << " select1 iteration " << i);
    }

    auto commit = co_await TSuspendWithFuture(tx.Commit(), context.TaskQueue, context.TerminalID);
    if (commit.Outcome != ECommitOutcome::Committed) {
        co_return false;
    }

    latency = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startTs);
    co_return true;
}

} // namespace NTpcc

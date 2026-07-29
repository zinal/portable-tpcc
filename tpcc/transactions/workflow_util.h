#pragma once

#include "context.h"
#include "ops.h"
#include "session.h"

#include <coro_traits.h>
#include <domain_util.h>
#include <log.h>
#include <task_queue.h>

#include <utility>

namespace NTpcc {

inline void ThrowIfRetryable(const TOperationResult& r) {
    if (r.Ok) {
        return;
    }
    if (MayBlindRetry(r.ErrorClass) || r.ErrorClass == EErrorClass::AmbiguousCommit) {
        throw TClassifiedError(r.ErrorClass, r.NativeCode, r.Message);
    }
}

inline void ThrowIfCommitFailed(const TCommitResult& r) {
    if (r.Outcome == ECommitOutcome::Committed) {
        return;
    }
    throw TClassifiedError(r.ErrorClass, r.NativeCode,
                           r.Message.empty() ? "commit failed" : r.Message);
}

// Returns a task-queue awaitable (do NOT wrap in another TFuture coroutine).
inline auto SuspendExecute(
    ITpccTransaction& tx,
    TTransactionContext& context,
    const TSemanticOp& op)
{
    return TSuspendWithFuture(tx.Execute(op), context.TaskQueue, context.TerminalID);
}

inline auto SuspendCommit(ITpccTransaction& tx, TTransactionContext& context) {
    return TSuspendWithFuture(tx.Commit(), context.TaskQueue, context.TerminalID);
}

inline auto SuspendRollback(ITpccTransaction& tx, TTransactionContext& context) {
    return TSuspendWithFuture(tx.Rollback(), context.TaskQueue, context.TerminalID);
}

inline bool FailPermanent(size_t terminalId, const char* msg) {
    LOG_E("Terminal {} {}", terminalId, msg);
    RequestStopWithError();
    return false;
}

} // namespace NTpcc

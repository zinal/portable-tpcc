#pragma once

#include "context.h"
#include "ops.h"
#include "session.h"

#include <coro_traits.h>
#include <domain_util.h>
#include <log.h>
#include <task_queue.h>

#include <string_view>
#include <utility>
#include <vector>

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

// TPC-C §5.1.2 / §5.4.2: intentional unused-item New-Order counts only after a
// confirmed rollback. Unconfirmed outcomes must not become UserAborted.
inline void ThrowIfRollbackFailed(const TCommitResult& r) {
    if (r.Outcome == ECommitOutcome::RolledBack) {
        return;
    }
    throw TClassifiedError(r.ErrorClass, r.NativeCode,
                           r.Message.empty() ? "rollback failed" : r.Message);
}

// Cardinality miss on ITEM is Integrity (adapter-api §4.3.1). Other classes are
// adapter/DBMS failures and must not be treated as the unused-item profile.
inline bool IsExpectedItemNotFound(const TOperationResult& r) {
    return !r.Ok && r.ErrorClass == EErrorClass::Integrity;
}

// Returns a task-queue awaitable (do NOT wrap in another TFuture coroutine).
inline auto SuspendExecute(
    ITpccTransaction& tx,
    TTransactionContext& context,
    const TSemanticOp& op)
{
    return TSuspendWithFuture(tx.Execute(op), context.TaskQueue, context.TerminalID);
}

inline auto SuspendExecuteBatch(
    ITpccTransaction& tx,
    TTransactionContext& context,
    const std::vector<TSemanticOp>& ops)
{
    return TSuspendWithFuture(tx.ExecuteBatch(ops), context.TaskQueue, context.TerminalID);
}

inline auto SuspendExecuteFinalAndCommit(
    ITpccTransaction& tx,
    TTransactionContext& context,
    const TSemanticOp& op)
{
    return TSuspendWithFuture(tx.ExecuteFinalAndCommit(op), context.TaskQueue, context.TerminalID);
}

inline void ThrowIfBatchRetryable(const TBatchResult& batch) {
    if (batch.Ok) {
        return;
    }
    TOperationResult r;
    r.Ok = false;
    r.ErrorClass = batch.ErrorClass;
    r.NativeCode = batch.NativeCode;
    r.Message = batch.Message;
    ThrowIfRetryable(r);
}

inline auto SuspendCommit(ITpccTransaction& tx, TTransactionContext& context) {
    return TSuspendWithFuture(tx.Commit(), context.TaskQueue, context.TerminalID);
}

inline auto SuspendRollback(ITpccTransaction& tx, TTransactionContext& context) {
    return TSuspendWithFuture(tx.Rollback(), context.TaskQueue, context.TerminalID);
}

inline void ThrowIfFinalCommitFailed(const TFinalCommitResult& r) {
    ThrowIfRetryable(r.Operation);
    if (!r.Operation.Ok) {
        throw TClassifiedError(
            r.Operation.ErrorClass,
            r.Operation.NativeCode,
            r.Operation.Message.empty() ? "final operation failed" : r.Operation.Message);
    }
    ThrowIfCommitFailed(r.Commit);
}

// Logs a failed operation. Stops the run only for Integrity (adapter-api §4.5).
// Permanent / cancelled / other classes return false so the terminal counts
// Fail and continues.
inline bool FailPermanent(
    size_t terminalId,
    const char* msg,
    std::string_view detail,
    EErrorClass errorClass)
{
    if (detail.empty()) {
        LOG_E("Terminal " << terminalId << " " << msg);
    } else {
        LOG_E("Terminal " << terminalId << " " << msg << ": " << detail);
    }
    if (AbortsRun(errorClass)) {
        RequestStopWithError();
    }
    return false;
}

inline bool FailPermanent(size_t terminalId, const char* msg) {
    return FailPermanent(terminalId, msg, {}, EErrorClass::Integrity);
}

inline bool FailPermanent(size_t terminalId, const char* msg, const TOperationResult& r) {
    return FailPermanent(terminalId, msg, r.Message, r.ErrorClass);
}

inline bool FailPermanent(size_t terminalId, const char* msg, const TBatchResult& r) {
    return FailPermanent(terminalId, msg, r.Message, r.ErrorClass);
}

} // namespace NTpcc

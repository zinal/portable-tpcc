#pragma once

#include "ops.h"

#include <future.h>

#include <memory>
#include <string>
#include <vector>

namespace NTpcc {

enum class EIsolationLevel {
    ReadCommitted,
    RepeatableRead,
    Serializable,
};

enum class ECommitOutcome {
    Committed,
    RolledBack,
    OutcomeUnknown,
};

enum class EErrorClass {
    RetryableAbort,
    NotCommitted,
    AmbiguousCommit,
    Permanent,
    Integrity,
    Cancelled,
};

struct TCommitResult {
    ECommitOutcome Outcome = ECommitOutcome::OutcomeUnknown;
    EErrorClass ErrorClass = EErrorClass::Permanent;
    std::string NativeCode;
    std::string Message;
};

struct TOperationResult {
    bool Ok = false;
    size_t ExpectedRows = 0;
    size_t ActualRows = 0;
    EErrorClass ErrorClass = EErrorClass::Permanent;
    std::string NativeCode;
    std::string Message;
    TOperationPayload Payload;
};

struct TBatchResult {
    bool Ok = false;
    EErrorClass ErrorClass = EErrorClass::Permanent;
    std::string NativeCode;
    std::string Message;
    std::vector<TOperationResult> Results;
};

struct TFinalCommitResult {
    TOperationResult Operation;
    TCommitResult Commit;
};

class ITpccTransaction {
public:
    virtual ~ITpccTransaction() = default;

    virtual TFuture<TOperationResult> Execute(const TSemanticOp& op) = 0;
    virtual TFuture<TBatchResult> ExecuteBatch(const std::vector<TSemanticOp>& ops) = 0;
    virtual TFuture<TFinalCommitResult> ExecuteFinalAndCommit(const TSemanticOp& op) = 0;
    virtual TFuture<TCommitResult> Commit() = 0;
    virtual TFuture<TCommitResult> Rollback() = 0;
    virtual TFuture<TCommitResult> Cancel() = 0;

    // Probe used by --simulate-select1 only. Default fails; adapters override.
    virtual TFuture<TOperationResult> ExecuteSelect1();
};

class ITpccSession {
public:
    virtual ~ITpccSession() = default;

    virtual TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel isolation) = 0;
};

class ISessionFactory {
public:
    virtual ~ISessionFactory() = default;

    // MAY be synchronous when construction does not block on the DBMS.
    virtual std::unique_ptr<ITpccSession> CreateSession() = 0;

    // Non-blocking acquire. Returns nullptr when no session is available now.
    // Adapters without a free-list pool (e.g. YDB) SHOULD implement this as
    // CreateSession() so the terminal never spins.
    virtual std::unique_ptr<ITpccSession> TryCreateSession() {
        return CreateSession();
    }
};

} // namespace NTpcc

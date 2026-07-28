#pragma once

#include <string>

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
};

class ITpccTransaction {
public:
    virtual ~ITpccTransaction() = default;

    virtual TOperationResult ExecuteSemantic(const void* operation, size_t operationSize) = 0;
    virtual TCommitResult Commit() = 0;
    virtual TCommitResult Rollback() = 0;
    virtual TCommitResult Cancel() = 0;
};

class ITpccSession {
public:
    virtual ~ITpccSession() = default;

    virtual std::unique_ptr<ITpccTransaction> Begin(EIsolationLevel isolation) = 0;
};

class ISessionFactory {
public:
    virtual ~ISessionFactory() = default;

    virtual std::unique_ptr<ITpccSession> CreateSession() = 0;
};

} // namespace NTpcc

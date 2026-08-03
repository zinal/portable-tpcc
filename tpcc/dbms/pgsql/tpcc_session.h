#pragma once

#include "pg_connection_pool.h"
#include "pg_error_classifier.h"
#include "pg_session.h"

#include <session.h>

#include <memory>

namespace NTpcc {

class TPgTpccTransaction : public ITpccTransaction {
public:
    explicit TPgTpccTransaction(PgSession& session);

    TFuture<TOperationResult> Execute(const TSemanticOp& op) override;
    TFuture<TBatchResult> ExecuteBatch(const std::vector<TSemanticOp>& ops) override;
    TFuture<TFinalCommitResult> ExecuteFinalAndCommit(const TSemanticOp& op) override;
    TFuture<TCommitResult> Commit() override;
    TFuture<TCommitResult> Rollback() override;
    TFuture<TCommitResult> Cancel() override;

    TFuture<TOperationResult> ExecuteSelect1();

private:
    bool TerminalState() const;

    PgSession& Session_;
    TPgErrorClassifier Classifier_;
    bool Terminal_ = false;
};

class TPgTpccSession : public ITpccSession {
public:
    explicit TPgTpccSession(PgSession& session);

    TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel isolation) override;

private:
    PgSession& Session_;
};

// Factory that acquires a pooled connection per CreateSession() call.
// The returned session owns the SessionGuard for the connection lifetime.
class TPgSessionFactory : public ISessionFactory {
public:
    explicit TPgSessionFactory(PgConnectionPool& pool);

    std::unique_ptr<ITpccSession> CreateSession() override;

private:
    PgConnectionPool& Pool_;
};

} // namespace NTpcc

#pragma once

#include "ob_connection_pool.h"
#include "ob_error_classifier.h"
#include "ob_session.h"

#include <session.h>

#include <memory>

namespace NTpcc {

class TObTpccTransaction : public ITpccTransaction {
public:
    explicit TObTpccTransaction(TObSession& session);

    TFuture<TOperationResult> Execute(const TSemanticOp& op) override;
    TFuture<TBatchResult> ExecuteBatch(const std::vector<TSemanticOp>& ops) override;
    TFuture<TFinalCommitResult> ExecuteFinalAndCommit(const TSemanticOp& op) override;
    TFuture<TCommitResult> Commit() override;
    TFuture<TCommitResult> Rollback() override;
    TFuture<TCommitResult> Cancel() override;

    TFuture<TOperationResult> ExecuteSelect1() override;

private:
    TObSession& Session_;
    TObErrorClassifier Classifier_;
    bool Terminal_ = false;
};

class TObTpccSession : public ITpccSession {
public:
    explicit TObTpccSession(TObSession& session);

    TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel isolation) override;

private:
    TObSession& Session_;
};

class TObSessionFactory : public ISessionFactory {
public:
    explicit TObSessionFactory(TObConnectionPool& pool);

    std::unique_ptr<ITpccSession> CreateSession() override;

private:
    TObConnectionPool& Pool_;
};

} // namespace NTpcc

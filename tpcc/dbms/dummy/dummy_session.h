#pragma once

#include "dummy_delay.h"

#include <session.h>

#include <memory>

namespace NTpcc {

class TDummyTpccTransaction : public ITpccTransaction {
public:
    TDummyTpccTransaction(TDummyDelayConfig delay, IExecutor* executor);

    TFuture<TOperationResult> Execute(const TSemanticOp& op) override;
    TFuture<TBatchResult> ExecuteBatch(const std::vector<TSemanticOp>& ops) override;
    TFuture<TFinalCommitResult> ExecuteFinalAndCommit(const TSemanticOp& op) override;
    TFuture<TCommitResult> Commit() override;
    TFuture<TCommitResult> Rollback() override;
    TFuture<TCommitResult> Cancel() override;
    TFuture<TOperationResult> ExecuteSelect1() override;

private:
    TOperationResult ExecuteImpl(const TSemanticOp& op) const;
    TCommitResult CommitImpl(ECommitOutcome outcome) const;
    template <typename T>
    TFuture<T> Delayed(T value) const;

    TDummyDelayConfig Delay_;
    IExecutor* Executor_ = nullptr;
    bool Terminal_ = false;
};

class TDummyTpccSession : public ITpccSession {
public:
    TDummyTpccSession(TDummyDelayConfig delay, IExecutor* executor);

    TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel isolation) override;

private:
    TDummyDelayConfig Delay_;
    IExecutor* Executor_ = nullptr;
};

class TDummySessionFactory : public ISessionFactory {
public:
    explicit TDummySessionFactory(TDummyDelayConfig delay, size_t ioThreads = 1);

    std::unique_ptr<ITpccSession> CreateSession() override;

private:
    TDummyDelayConfig Delay_;
    std::unique_ptr<TThreadPool> Pool_;
    IExecutor* Executor_ = nullptr;
};

} // namespace NTpcc

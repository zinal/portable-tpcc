#pragma once

#include "ydb_driver.h"
#include "ydb_error_classifier.h"

#include <session.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>

#include <memory>
#include <optional>

namespace NTpcc {

class TYdbTpccTransaction : public ITpccTransaction {
public:
    TYdbTpccTransaction(NYdb::NQuery::TSession session, std::string path);

    TFuture<TOperationResult> Execute(const TSemanticOp& op) override;
    TFuture<TBatchResult> ExecuteBatch(const std::vector<TSemanticOp>& ops) override;
    TFuture<TFinalCommitResult> ExecuteFinalAndCommit(const TSemanticOp& op) override;
    TFuture<TCommitResult> Commit() override;
    TFuture<TCommitResult> Rollback() override;
    TFuture<TCommitResult> Cancel() override;

    TFuture<TOperationResult> ExecuteSelect1();

private:
    NYdb::NQuery::TSession Session_;
    std::optional<NYdb::NQuery::TTransaction> Tx_;
    std::string Path_;
    TYdbErrorClassifier Classifier_;
    bool Terminal_ = false;
    bool FinalCommitMode_ = false;
};

class TYdbTpccSession : public ITpccSession {
public:
    TYdbTpccSession(NYdb::NQuery::TSession session, std::string path);

    TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel isolation) override;

private:
    NYdb::NQuery::TSession Session_;
    std::string Path_;
};

class TYdbSessionFactory : public ISessionFactory {
public:
    explicit TYdbSessionFactory(TYdbConnection& connection);

    std::unique_ptr<ITpccSession> CreateSession() override;

private:
    TYdbConnection& Connection_;
};

} // namespace NTpcc

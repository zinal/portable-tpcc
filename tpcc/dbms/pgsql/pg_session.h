#pragma once

#include "pqxx_compat.h"
#include "query_result.h"
#include <future.h>
#include <thread_pool.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace NTpcc {

using SnapshotTxn = pqxx::transaction<pqxx::isolation_level::repeatable_read>;

class PgSession {
public:
    PgSession() = default;
    PgSession(std::unique_ptr<pqxx::connection> conn, IExecutor* executor,
              std::shared_ptr<std::atomic<bool>> shutdownFlag = {});

    PgSession(PgSession&& other) noexcept;
    PgSession& operator=(PgSession&& other) noexcept;

    PgSession(const PgSession&) = delete;
    PgSession& operator=(const PgSession&) = delete;

    ~PgSession();

    TFuture<QueryResult> ExecuteQuery(std::string_view sql);

    template<typename... Args>
    TFuture<QueryResult> ExecuteQuery(std::string_view sql, Args&&... args) {
        return ExecuteQueryImpl(sql, std::forward<Args>(args)...);
    }

    TFuture<uint64_t> ExecuteModify(std::string_view sql);

    template<typename... Args>
    TFuture<uint64_t> ExecuteModify(std::string_view sql, Args&&... args) {
        return ExecuteModifyImpl(sql, std::forward<Args>(args)...);
    }

    TFuture<void> Commit();
    TFuture<void> Rollback();

    TFuture<QueryResult> ExecuteNonTx(std::string_view sql);

    TFuture<void> ExecuteCopy(
        const std::string& tableName,
        const std::vector<std::string>& columns,
        std::function<void(pqxx::stream_to&)> writer);

    bool HasConnection() const { return conn_ != nullptr; }
    pqxx::connection& GetRawConnection() { return *conn_; }

    std::unique_ptr<pqxx::connection> ReleaseConnection();

    void SetShutdownFlag(std::shared_ptr<std::atomic<bool>> flag) { shutdownFlag_ = std::move(flag); }

private:
    void CheckShutdown() const;

    template<typename... Args>
    TFuture<QueryResult> ExecuteQueryImpl(std::string_view sql, Args&&... args);

    template<typename... Args>
    TFuture<uint64_t> ExecuteModifyImpl(std::string_view sql, Args&&... args);

    std::unique_ptr<pqxx::connection> conn_;
    std::unique_ptr<SnapshotTxn> txn_;
    IExecutor* executor_ = nullptr;
    std::shared_ptr<std::atomic<bool>> shutdownFlag_;
};

template<typename... Args>
TFuture<QueryResult> PgSession::ExecuteQueryImpl(std::string_view sql, Args&&... args) {
    TPromise<QueryResult> promise;
    auto future = promise.GetFuture();
    std::string sqlCopy(sql);
    auto boundArgs = std::make_tuple(std::forward<Args>(args)...);

    executor_->Submit([this, sqlCopy = std::move(sqlCopy), boundArgs = std::move(boundArgs),
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            if (!txn_) {
                txn_ = std::make_unique<SnapshotTxn>(*conn_);
            }
            auto result = std::apply(
                [&](auto&... params) {
                    return txn_->exec_params(sqlCopy, params...);
                },
                boundArgs);
            p.SetValue(QueryResult(std::move(result)));
        } catch (...) {
            p.SetException(std::current_exception());
        }
    });

    return future;
}

template<typename... Args>
TFuture<uint64_t> PgSession::ExecuteModifyImpl(std::string_view sql, Args&&... args) {
    TPromise<uint64_t> promise;
    auto future = promise.GetFuture();
    std::string sqlCopy(sql);
    auto boundArgs = std::make_tuple(std::forward<Args>(args)...);

    executor_->Submit([this, sqlCopy = std::move(sqlCopy), boundArgs = std::move(boundArgs),
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            if (!txn_) {
                txn_ = std::make_unique<SnapshotTxn>(*conn_);
            }
            auto result = std::apply(
                [&](auto&... params) {
                    return txn_->exec_params(sqlCopy, params...);
                },
                boundArgs);
            p.SetValue(result.affected_rows());
        } catch (...) {
            p.SetException(std::current_exception());
        }
    });

    return future;
}

} // namespace NTpcc

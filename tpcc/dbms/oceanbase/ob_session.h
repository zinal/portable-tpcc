#pragma once

#include "ob_params.h"
#include "ob_queries.h"
#include "query_result.h"

#include <future.h>
#include <thread_pool.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NTpcc {

struct TObConnection;

using TObBulkRow = std::vector<std::optional<std::string>>;
using TObBulkWriter = std::function<void(std::function<void(TObBulkRow)>)>;

class TObSession {
public:
    TObSession() = default;
    TObSession(
        std::unique_ptr<TObConnection> conn,
        IExecutor* executor,
        std::shared_ptr<std::atomic<bool>> shutdownFlag = {});

    TObSession(TObSession&& other) noexcept;
    TObSession& operator=(TObSession&& other) noexcept;

    TObSession(const TObSession&) = delete;
    TObSession& operator=(const TObSession&) = delete;

    ~TObSession();

    TFuture<QueryResult> ExecuteQuery(EObQueryId queryId, const TObParams& params = {});
    TFuture<uint64_t> ExecuteModify(EObQueryId queryId, const TObParams& params = {});

    TFuture<QueryResult> ExecuteQuery(std::string_view sql, const TObParams& params = {});
    TFuture<uint64_t> ExecuteModify(std::string_view sql, const TObParams& params = {});
    TFuture<void> Commit();
    TFuture<void> Rollback();

    TFuture<QueryResult> ExecuteNonTx(std::string_view sql);

    TFuture<void> ExecuteBulk(
        const std::string& tableName,
        const std::vector<std::string>& columns,
        TObBulkWriter writer);

    bool HasConnection() const;
    bool IsReusable() const;
    std::unique_ptr<TObConnection> ReleaseConnection(bool* reusable = nullptr);

    void SetShutdownFlag(std::shared_ptr<std::atomic<bool>> flag) {
        ShutdownFlag_ = std::move(flag);
    }

private:
    void CheckShutdown() const;
    void MarkException(const std::exception& ex);

    std::unique_ptr<TObConnection> Conn_;
    bool InTxn_ = false;
    IExecutor* Executor_ = nullptr;
    std::shared_ptr<std::atomic<bool>> ShutdownFlag_;
    bool Broken_ = false;
};

} // namespace NTpcc

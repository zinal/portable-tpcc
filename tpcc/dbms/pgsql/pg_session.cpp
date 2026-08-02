#include "pg_session.h"
#include <log.h>

namespace NTpcc {

namespace {

bool IsBrokenConnectionException(const std::exception& ex) {
    if (dynamic_cast<const pqxx::broken_connection*>(&ex) != nullptr) {
        return true;
    }
    if (const auto* sql = dynamic_cast<const pqxx::sql_error*>(&ex)) {
        const std::string state = sql->sqlstate();
        return state.size() >= 2 && state.substr(0, 2) == "08";
    }
    return false;
}

} // anonymous

PgSession::PgSession(std::unique_ptr<pqxx::connection> conn, IExecutor* executor,
                     std::shared_ptr<std::atomic<bool>> shutdownFlag)
    : conn_(std::move(conn))
    , executor_(executor)
    , shutdownFlag_(std::move(shutdownFlag))
{}

PgSession::PgSession(PgSession&& other) noexcept
    : conn_(std::move(other.conn_))
    , txn_(std::move(other.txn_))
    , executor_(other.executor_)
    , shutdownFlag_(std::move(other.shutdownFlag_))
    , broken_(other.broken_)
{
    other.executor_ = nullptr;
    other.broken_ = true;
}

PgSession& PgSession::operator=(PgSession&& other) noexcept {
    if (this != &other) {
        conn_ = std::move(other.conn_);
        txn_ = std::move(other.txn_);
        executor_ = other.executor_;
        shutdownFlag_ = std::move(other.shutdownFlag_);
        broken_ = other.broken_;
        other.executor_ = nullptr;
        other.broken_ = true;
    }
    return *this;
}

void PgSession::CheckShutdown() const {
    if (shutdownFlag_ && shutdownFlag_->load(std::memory_order_relaxed)) {
        throw std::runtime_error("session shutdown");
    }
}

void PgSession::MarkException(const std::exception& ex) {
    if (IsBrokenConnectionException(ex)) {
        broken_ = true;
    }
}

PgSession::~PgSession() {
    if (txn_) {
        try {
            txn_->abort();
        } catch (...) {
        }
        txn_.reset();
    }
}

bool PgSession::IsReusable() const {
    return conn_ && !broken_ && conn_->is_open();
}

TFuture<QueryResult> PgSession::ExecuteQuery(std::string_view sql) {
    TPromise<QueryResult> promise;
    auto future = promise.GetFuture();
    std::string sqlCopy(sql);

    executor_->Submit([this, sqlCopy = std::move(sqlCopy),
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            if (!txn_) {
                txn_ = std::make_unique<SnapshotTxn>(*conn_);
            }
            auto result = txn_->exec(sqlCopy);
            p.SetValue(QueryResult(std::move(result)));
        } catch (const std::exception& ex) {
            MarkException(ex);
            p.SetException(std::current_exception());
        } catch (...) {
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<uint64_t> PgSession::ExecuteModify(std::string_view sql) {
    TPromise<uint64_t> promise;
    auto future = promise.GetFuture();
    std::string sqlCopy(sql);

    executor_->Submit([this, sqlCopy = std::move(sqlCopy),
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            if (!txn_) {
                txn_ = std::make_unique<SnapshotTxn>(*conn_);
            }
            auto result = txn_->exec(sqlCopy);
            p.SetValue(result.affected_rows());
        } catch (const std::exception& ex) {
            MarkException(ex);
            p.SetException(std::current_exception());
        } catch (...) {
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<void> PgSession::Commit() {
    TPromise<void> promise;
    auto future = promise.GetFuture();

    executor_->Submit([this, p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            if (txn_) {
                txn_->commit();
                txn_.reset();
            }
            p.SetValue();
        } catch (const std::exception& ex) {
            txn_.reset();
            MarkException(ex);
            p.SetException(std::current_exception());
        } catch (...) {
            txn_.reset();
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<void> PgSession::Rollback() {
    TPromise<void> promise;
    auto future = promise.GetFuture();

    executor_->Submit([this, p = std::move(promise)]() mutable {
        try {
            if (txn_) {
                txn_->abort();
                txn_.reset();
            }
            p.SetValue();
        } catch (const std::exception& ex) {
            txn_.reset();
            MarkException(ex);
            p.SetException(std::current_exception());
        } catch (...) {
            txn_.reset();
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<QueryResult> PgSession::ExecuteNonTx(std::string_view sql) {
    TPromise<QueryResult> promise;
    auto future = promise.GetFuture();
    std::string sqlCopy(sql);

    executor_->Submit([this, sqlCopy = std::move(sqlCopy),
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            pqxx::nontransaction ntx(*conn_);
            auto result = ntx.exec(sqlCopy);
            p.SetValue(QueryResult(std::move(result)));
        } catch (const std::exception& ex) {
            MarkException(ex);
            p.SetException(std::current_exception());
        } catch (...) {
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<void> PgSession::ExecuteCopy(
    const std::string& tableName,
    const std::vector<std::string>& columns,
    std::function<void(pqxx::stream_to&)> writer)
{
    TPromise<void> promise;
    auto future = promise.GetFuture();

    executor_->Submit([this, tableName, columns, writer = std::move(writer),
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            if (!txn_) {
                txn_ = std::make_unique<SnapshotTxn>(*conn_);
            }
            auto stream = pqxx::stream_to(*txn_, tableName, columns);
            writer(stream);
            stream.complete();
            p.SetValue();
        } catch (const std::exception& ex) {
            MarkException(ex);
            p.SetException(std::current_exception());
        } catch (...) {
            p.SetException(std::current_exception());
        }
    });

    return future;
}

std::unique_ptr<pqxx::connection> PgSession::ReleaseConnection(bool* reusable) {
    if (txn_) {
        try {
            txn_->abort();
        } catch (const std::exception& ex) {
            MarkException(ex);
        } catch (...) {
        }
        txn_.reset();
    }
    if (reusable) {
        *reusable = IsReusable();
    }
    return std::move(conn_);
}

} // namespace NTpcc

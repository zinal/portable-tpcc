#include "ob_session.h"

#include "ob_connection.h"
#include "ob_errors.h"

#include <put_batch.h>

#include <stdexcept>

namespace NTpcc {

namespace {

void EnsureTxn(TObConnection& conn, bool& inTxn) {
    if (!inTxn) {
        conn.BeginRepeatableRead();
        inTxn = true;
    }
}

void ResetTxnOnError(TObConnection* conn, bool& inTxn) {
    if (!conn || !inTxn) {
        return;
    }
    try {
        conn->Rollback();
    } catch (...) {
    }
    inTxn = false;
}

bool IsBrokenConnectionException(const std::exception& ex) {
    if (const auto* db = dynamic_cast<const TObDbError*>(&ex)) {
        return db->Kind() == EObDbErrorKind::ConnectionLost;
    }
    return false;
}

} // namespace

TObSession::TObSession(
    std::unique_ptr<TObConnection> conn,
    IExecutor* executor,
    std::shared_ptr<std::atomic<bool>> shutdownFlag)
    : Conn_(std::move(conn))
    , Executor_(executor)
    , ShutdownFlag_(std::move(shutdownFlag))
{}

TObSession::TObSession(TObSession&& other) noexcept
    : Conn_(std::move(other.Conn_))
    , InTxn_(other.InTxn_)
    , Executor_(other.Executor_)
    , ShutdownFlag_(std::move(other.ShutdownFlag_))
    , Broken_(other.Broken_)
    , LastErrorCode_(other.LastErrorCode_)
    , LastErrorKind_(other.LastErrorKind_)
    , LastErrorMessage_(std::move(other.LastErrorMessage_))
{
    other.InTxn_ = false;
    other.Executor_ = nullptr;
    other.Broken_ = true;
    other.LastErrorCode_ = 0;
    other.LastErrorKind_ = EObDbErrorKind::Other;
}

TObSession& TObSession::operator=(TObSession&& other) noexcept {
    if (this != &other) {
        Conn_ = std::move(other.Conn_);
        InTxn_ = other.InTxn_;
        Executor_ = other.Executor_;
        ShutdownFlag_ = std::move(other.ShutdownFlag_);
        Broken_ = other.Broken_;
        LastErrorCode_ = other.LastErrorCode_;
        LastErrorKind_ = other.LastErrorKind_;
        LastErrorMessage_ = std::move(other.LastErrorMessage_);
        other.InTxn_ = false;
        other.Executor_ = nullptr;
        other.Broken_ = true;
        other.LastErrorCode_ = 0;
        other.LastErrorKind_ = EObDbErrorKind::Other;
    }
    return *this;
}

TObSession::~TObSession() {
    if (Conn_ && InTxn_) {
        try {
            Conn_->Rollback();
        } catch (...) {
        }
        InTxn_ = false;
    }
}

void TObSession::CheckShutdown() const {
    if (ShutdownFlag_ && ShutdownFlag_->load(std::memory_order_relaxed)) {
        throw std::runtime_error("session shutdown");
    }
}

void TObSession::MarkException(const std::exception& ex) {
    // Keep the first tenant-memory error: a follow-up 2013 on rollback
    // would otherwise hide OB -4013 and trigger a reconnect storm.
    if (LastErrorKind_ != EObDbErrorKind::TenantMemoryLimit) {
        LastErrorMessage_ = ex.what();
        if (const auto* db = dynamic_cast<const TObDbError*>(&ex)) {
            LastErrorCode_ = db->Code();
            LastErrorKind_ = db->Kind();
        } else {
            LastErrorCode_ = 0;
            LastErrorKind_ = EObDbErrorKind::Other;
        }
    }
    if (IsBrokenConnectionException(ex)) {
        Broken_ = true;
    }
}

bool TObSession::HasConnection() const {
    return Conn_ != nullptr && Conn_->Ok();
}

bool TObSession::IsReusable() const {
    return Conn_ && !Broken_ && Conn_->Ok();
}

std::unique_ptr<TObConnection> TObSession::ReleaseConnection(bool* reusable) {
    if (Conn_ && InTxn_) {
        try {
            Conn_->Rollback();
        } catch (const std::exception& ex) {
            MarkException(ex);
        } catch (...) {
        }
        InTxn_ = false;
    }
    if (reusable) {
        *reusable = IsReusable();
    }
    return std::move(Conn_);
}

TFuture<QueryResult> TObSession::ExecuteQuery(EObQueryId queryId, const TObParams& params) {
    TPromise<QueryResult> promise;
    auto future = promise.GetFuture();

    Executor_->Submit([this, queryId, params, p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            EnsureTxn(*Conn_, InTxn_);
            p.SetValue(Conn_->Query(queryId, params));
        } catch (const std::exception& ex) {
            MarkException(ex);
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        } catch (...) {
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<uint64_t> TObSession::ExecuteModify(EObQueryId queryId, const TObParams& params) {
    TPromise<uint64_t> promise;
    auto future = promise.GetFuture();

    Executor_->Submit([this, queryId, params, p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            EnsureTxn(*Conn_, InTxn_);
            p.SetValue(Conn_->Execute(queryId, params));
        } catch (const std::exception& ex) {
            MarkException(ex);
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        } catch (...) {
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<QueryResult> TObSession::ExecuteQuery(std::string_view sql, const TObParams& params) {
    TPromise<QueryResult> promise;
    auto future = promise.GetFuture();
    std::string sqlCopy(sql);

    Executor_->Submit([this, sqlCopy = std::move(sqlCopy), params,
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            EnsureTxn(*Conn_, InTxn_);
            p.SetValue(Conn_->Query(sqlCopy, params));
        } catch (const std::exception& ex) {
            MarkException(ex);
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        } catch (...) {
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<uint64_t> TObSession::ExecuteModify(std::string_view sql, const TObParams& params) {
    TPromise<uint64_t> promise;
    auto future = promise.GetFuture();
    std::string sqlCopy(sql);

    Executor_->Submit([this, sqlCopy = std::move(sqlCopy), params,
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            EnsureTxn(*Conn_, InTxn_);
            p.SetValue(Conn_->Execute(sqlCopy, params));
        } catch (const std::exception& ex) {
            MarkException(ex);
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        } catch (...) {
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<TObMultiResult> TObSession::ExecuteMulti(std::string sql, bool finishesTransaction) {
    TPromise<TObMultiResult> promise;
    auto future = promise.GetFuture();

    Executor_->Submit([this, sql = std::move(sql), finishesTransaction,
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            EnsureTxn(*Conn_, InTxn_);
            auto result = Conn_->ExecuteMulti(sql);
            if (finishesTransaction) {
                InTxn_ = false;
            }
            p.SetValue(std::move(result));
        } catch (const std::exception& ex) {
            MarkException(ex);
            // COMMIT is the last statement of a finish script. Connection loss
            // may mean the server already committed; do not ROLLBACK. Other
            // errors happen before COMMIT and must release locks.
            if (finishesTransaction && IsBrokenConnectionException(ex)) {
                InTxn_ = false;
            } else {
                ResetTxnOnError(Conn_.get(), InTxn_);
            }
            p.SetException(std::current_exception());
        } catch (...) {
            if (finishesTransaction) {
                InTxn_ = false;
            } else {
                ResetTxnOnError(Conn_.get(), InTxn_);
            }
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<void> TObSession::Commit() {
    TPromise<void> promise;
    auto future = promise.GetFuture();

    Executor_->Submit([this, p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            if (InTxn_) {
                Conn_->Commit();
                InTxn_ = false;
            }
            p.SetValue();
        } catch (const std::exception& ex) {
            InTxn_ = false;
            MarkException(ex);
            p.SetException(std::current_exception());
        } catch (...) {
            InTxn_ = false;
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<void> TObSession::Rollback() {
    TPromise<void> promise;
    auto future = promise.GetFuture();

    Executor_->Submit([this, p = std::move(promise)]() mutable {
        try {
            if (InTxn_) {
                Conn_->Rollback();
                InTxn_ = false;
            }
            p.SetValue();
        } catch (const std::exception& ex) {
            InTxn_ = false;
            MarkException(ex);
            p.SetException(std::current_exception());
        } catch (...) {
            InTxn_ = false;
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<QueryResult> TObSession::ExecuteNonTx(std::string_view sql) {
    TPromise<QueryResult> promise;
    auto future = promise.GetFuture();
    std::string sqlCopy(sql);

    Executor_->Submit([this, sqlCopy = std::move(sqlCopy),
                       p = std::move(promise)]() mutable {
        try {
            if (InTxn_) {
                Conn_->Rollback();
                InTxn_ = false;
            }
            p.SetValue(Conn_->QuerySimple(sqlCopy));
        } catch (const std::exception& ex) {
            MarkException(ex);
            p.SetException(std::current_exception());
        } catch (...) {
            p.SetException(std::current_exception());
        }
    });

    return future;
}

TFuture<void> TObSession::ExecuteBulk(
    const std::string& tableName,
    const std::vector<std::string>& columns,
    TObBulkWriter writer)
{
    TPromise<void> promise;
    auto future = promise.GetFuture();

    Executor_->Submit([this, tableName, columns, writer = std::move(writer),
                       p = std::move(promise)]() mutable {
        try {
            CheckShutdown();
            EnsureTxn(*Conn_, InTxn_);

            constexpr size_t BULK_BATCH_ROWS = static_cast<size_t>(DEFAULT_LOAD_BATCH_ROWS);
            std::vector<TObBulkRow> batch;
            batch.reserve(BULK_BATCH_ROWS);
            std::string cachedSql;
            size_t cachedRowCount = 0;

            auto flush = [&]() {
                if (batch.empty()) {
                    return;
                }
                if (batch.size() != cachedRowCount) {
                    cachedSql = "INSERT INTO " + QuoteIdent(tableName) + " (";
                    for (size_t i = 0; i < columns.size(); ++i) {
                        if (i) cachedSql += ',';
                        cachedSql += QuoteIdent(columns[i]);
                    }
                    cachedSql += ") VALUES ";
                    for (size_t r = 0; r < batch.size(); ++r) {
                        if (r) cachedSql += ',';
                        cachedSql += '(';
                        for (size_t c = 0; c < columns.size(); ++c) {
                            if (c) cachedSql += ',';
                            cachedSql += '?';
                        }
                        cachedSql += ')';
                    }
                    cachedRowCount = batch.size();
                }

                TObParams params;
                params.Reserve(batch.size() * columns.size());
                for (size_t r = 0; r < batch.size(); ++r) {
                    const auto& row = batch[r];
                    if (row.size() != columns.size()) {
                        throw std::runtime_error("bulk row column count mismatch");
                    }
                    for (size_t c = 0; c < row.size(); ++c) {
                        if (row[c]) {
                            params(*row[c]);
                        } else {
                            params(nullptr);
                        }
                    }
                }
                Conn_->Execute(cachedSql, params);
                batch.clear();
            };

            writer([&](TObBulkRow row) {
                batch.push_back(std::move(row));
                if (batch.size() >= BULK_BATCH_ROWS) {
                    flush();
                }
            });
            flush();
            p.SetValue();
        } catch (const std::exception& ex) {
            MarkException(ex);
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        } catch (...) {
            ResetTxnOnError(Conn_.get(), InTxn_);
            p.SetException(std::current_exception());
        }
    });

    return future;
}

} // namespace NTpcc

#include "pg_connection_pool.h"
#include <domain_util.h>
#include <log.h>

#include <fmt/format.h>

namespace NTpcc {

PgConnectionPool::PgConnectionPool(const std::string& connectionString,
                                   size_t poolSize,
                                   size_t ioThreads,
                                   const std::string& path)
    : connectionString_(connectionString)
    , path_(path)
    , poolSize_(poolSize)
    , executor_(std::make_unique<TThreadPool>(ioThreads))
{
    LOG_I("Creating connection pool: " << poolSize << " connections, " << ioThreads << " IO threads");

    for (size_t i = 0; i < poolSize; ++i) {
        connections_.push(CreateConnection());
    }

    LOG_I("Connection pool ready");
}

PgConnectionPool::~PgConnectionPool() {
    {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();

    executor_->Join();

    std::lock_guard lock(mutex_);
    while (!connections_.empty()) {
        connections_.pop();
    }
}

std::unique_ptr<pqxx::connection> PgConnectionPool::CreateConnection() const {
    auto conn = std::make_unique<pqxx::connection>(connectionString_);
    if (!path_.empty()) {
        pqxx::nontransaction ntx(*conn);
        ntx.exec(fmt::format("SET search_path TO {}", conn->quote_name(path_)));
    }
    return conn;
}

PgSession PgConnectionPool::AcquireSession() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return !connections_.empty() || shutdown_; });

    if (shutdown_) {
        throw std::runtime_error("Connection pool is shutting down");
    }

    auto conn = std::move(connections_.front());
    connections_.pop();
    checkedOut_.push_back(conn.get());
    return PgSession(std::move(conn), executor_.get(), sessionShutdownFlag_);
}

std::optional<PgSession> PgConnectionPool::TryAcquireSession() {
    std::lock_guard lock(mutex_);
    if (connections_.empty() || shutdown_) {
        return std::nullopt;
    }

    auto conn = std::move(connections_.front());
    connections_.pop();
    checkedOut_.push_back(conn.get());
    return PgSession(std::move(conn), executor_.get(), sessionShutdownFlag_);
}

void PgConnectionPool::ReleaseSession(PgSession session) {
    bool reusable = false;
    auto conn = session.ReleaseConnection(&reusable);
    if (!conn) return;
    pqxx::connection* released = conn.get();

    {
        std::lock_guard lock(mutex_);
        std::erase(checkedOut_, released);
        if (shutdown_) {
            return;
        }
    }

    std::unique_ptr<pqxx::connection> replacement;
    if (!reusable) {
        LOG_W("Dropping non-reusable PostgreSQL session and opening a replacement");
        try {
            replacement = CreateConnection();
        } catch (const std::exception& ex) {
            LOG_E("Failed to recreate PostgreSQL connection: " << ex.what());
            sessionShutdownFlag_->store(true, std::memory_order_release);
            {
                std::lock_guard lock(mutex_);
                shutdown_ = true;
            }
            RequestStopWithError();
            cv_.notify_all();
            return;
        }
    }

    {
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            return;
        }
        if (reusable) {
            connections_.push(std::move(conn));
        } else {
            connections_.push(std::move(replacement));
        }
    }
    cv_.notify_one();
}

void PgConnectionPool::CancelAll() {
    sessionShutdownFlag_->store(true, std::memory_order_release);

    std::lock_guard lock(mutex_);
    for (auto* conn : checkedOut_) {
        try {
            conn->cancel_query();
        } catch (...) {
        }
    }
}

PgConnectionPool::SessionGuard PgConnectionPool::AcquireGuard() {
    return SessionGuard(*this, AcquireSession());
}

std::optional<PgConnectionPool::SessionGuard> PgConnectionPool::TryAcquireGuard() {
    auto session = TryAcquireSession();
    if (!session) {
        return std::nullopt;
    }
    return SessionGuard(*this, std::move(*session));
}

} // namespace NTpcc

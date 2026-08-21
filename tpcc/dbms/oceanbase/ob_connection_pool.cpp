#include "ob_connection_pool.h"

#include <domain_util.h>
#include <log.h>

#include <algorithm>
#include <stdexcept>

namespace NTpcc {

TObConnectionPool::TObConnectionPool(
    const std::string& connectionString,
    size_t poolSize,
    size_t ioThreads,
    const std::string& path)
    : TObConnectionPool(ConfigWithPath(connectionString, path), poolSize, ioThreads)
{}

TObConnectionPool::TObConnectionPool(TObConnectionConfig config, size_t poolSize, size_t ioThreads)
    : Config_(std::move(config))
    , PoolSize_(poolSize)
    , Executor_(std::make_unique<TThreadPool>(ioThreads))
{
    LOG_I("Creating OceanBase connection pool: " << PoolSize_ << " connections, "
          << ioThreads << " IO threads");

    for (size_t i = 0; i < PoolSize_; ++i) {
        Connections_.push(CreateConnection());
    }

    LOG_I("Connection pool ready");
}

TObConnectionPool::~TObConnectionPool() {
    {
        std::lock_guard lock(Mutex_);
        Shutdown_ = true;
    }
    Cv_.notify_all();

    Executor_->Join();

    std::lock_guard lock(Mutex_);
    while (!Connections_.empty()) {
        Connections_.pop();
    }
}

std::unique_ptr<TObConnection> TObConnectionPool::CreateConnection() const {
    return TObConnection::Connect(Config_);
}

TObSession TObConnectionPool::AcquireSession() {
    std::unique_lock lock(Mutex_);
    Cv_.wait(lock, [this] { return !Connections_.empty() || Shutdown_; });

    if (Shutdown_) {
        throw std::runtime_error("Connection pool is shutting down");
    }

    auto conn = std::move(Connections_.front());
    Connections_.pop();
    CheckedOut_.push_back(conn.get());
    return TObSession(std::move(conn), Executor_.get(), ShutdownFlag_);
}

std::optional<TObSession> TObConnectionPool::TryAcquireSession() {
    std::lock_guard lock(Mutex_);
    if (Connections_.empty() || Shutdown_) {
        return std::nullopt;
    }

    auto conn = std::move(Connections_.front());
    Connections_.pop();
    CheckedOut_.push_back(conn.get());
    return TObSession(std::move(conn), Executor_.get(), ShutdownFlag_);
}

void TObConnectionPool::ReleaseSession(TObSession session) {
    bool reusable = false;
    auto conn = session.ReleaseConnection(&reusable);
    if (!conn) {
        return;
    }
    TObConnection* released = conn.get();

    {
        std::lock_guard lock(Mutex_);
        std::erase(CheckedOut_, released);
        if (Shutdown_) {
            return;
        }
    }

    std::unique_ptr<TObConnection> replacement;
    if (!reusable) {
        LOG_W("Dropping non-reusable OceanBase session and opening a replacement");
        try {
            replacement = CreateConnection();
        } catch (const std::exception& ex) {
            LOG_E("Failed to recreate OceanBase connection: " << ex.what());
            ShutdownFlag_->store(true, std::memory_order_release);
            {
                std::lock_guard lock(Mutex_);
                Shutdown_ = true;
            }
            RequestStopWithError();
            Cv_.notify_all();
            return;
        }
    }

    {
        std::lock_guard lock(Mutex_);
        if (Shutdown_) {
            return;
        }
        if (reusable) {
            Connections_.push(std::move(conn));
        } else {
            Connections_.push(std::move(replacement));
        }
    }
    Cv_.notify_one();
}

void TObConnectionPool::CancelAll() {
    ShutdownFlag_->store(true, std::memory_order_release);

    std::vector<TObConnection*> victims;
    {
        std::lock_guard lock(Mutex_);
        victims = CheckedOut_;
    }
    for (auto* conn : victims) {
        try {
            conn->KillQuery(Config_);
        } catch (...) {
        }
    }
}

TObConnectionPool::TSessionGuard TObConnectionPool::AcquireGuard() {
    return TSessionGuard(*this, AcquireSession());
}

std::optional<TObConnectionPool::TSessionGuard> TObConnectionPool::TryAcquireGuard() {
    auto session = TryAcquireSession();
    if (!session) {
        return std::nullopt;
    }
    return TSessionGuard(*this, std::move(*session));
}

} // namespace NTpcc

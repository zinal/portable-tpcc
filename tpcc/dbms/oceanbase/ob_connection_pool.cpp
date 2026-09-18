#include "ob_connection_pool.h"

#include "ob_errors.h"

#include <log.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace NTpcc {
namespace {

constexpr auto ReconnectInitialBackoff = std::chrono::milliseconds(100);
constexpr auto ReconnectMaxBackoff = std::chrono::milliseconds(2000);
constexpr auto TenantMemoryReconnectInitialBackoff = std::chrono::seconds(5);
constexpr auto TenantMemoryReconnectMaxBackoff = std::chrono::seconds(30);

const char* TenantMemoryHint() {
    return "OceanBase tenant is out of memory (OB -4013 / "
           "\"No memory or reach tenant memory limit\"). "
           "COM_STMT_PREPARE of worker SQL (for example Payment "
           "UPDATE warehouse) fails, and reconnecting retries PREPARE "
           "on every new session, which makes the limit worse. "
           "Increase tenant MEMORY_SIZE (ALTER RESOURCE UNIT / GV$OB_UNITS) "
           "or reduce max_inflight / connections.";
}

size_t MassReplacementThreshold(size_t poolSize) {
    return std::max<size_t>(8, poolSize / 8);
}

} // namespace

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
    std::deque<TPromise<TObSession>> pendingWaiters;
    {
        std::lock_guard lock(Mutex_);
        Shutdown_ = true;
        pendingWaiters.swap(Waiters_);
    }
    for (auto& waiter : pendingWaiters) {
        waiter.SetException(std::make_exception_ptr(
            TObDbError(1317, "Connection pool is shutting down")));
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

TFuture<TObSession> TObConnectionPool::AcquireSessionAsync() {
    TPromise<TObSession> promise;
    auto future = promise.GetFuture();

    std::unique_ptr<TObConnection> conn;
    bool shutdown = false;
    {
        std::lock_guard lock(Mutex_);
        if (Shutdown_) {
            shutdown = true;
        } else if (!Connections_.empty()) {
            conn = std::move(Connections_.front());
            Connections_.pop();
            CheckedOut_.push_back(conn.get());
        } else {
            // No connection available now; park the caller.
            Waiters_.push_back(std::move(promise));
            return future;
        }
    }

    if (shutdown) {
        promise.SetException(std::make_exception_ptr(
            TObDbError(1317, "Connection pool is shutting down")));
    } else {
        promise.SetValue(TObSession(std::move(conn), Executor_.get(), ShutdownFlag_));
    }
    return future;
}

void TObConnectionPool::ReleaseSession(TObSession session) {
    bool reusable = false;
    const int lastCode = session.LastErrorCode();
    const EObDbErrorKind lastKind = session.LastErrorKind();
    const std::string lastMsg = session.LastErrorMessage();
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

    if (reusable) {
        TPromise<TObSession> waiter;
        bool hasWaiter = false;
        {
            std::lock_guard lock(Mutex_);
            if (Shutdown_) {
                return;
            }
            if (!Waiters_.empty()) {
                waiter = std::move(Waiters_.front());
                Waiters_.pop_front();
                hasWaiter = true;
                // Keep the connection as "checked out" for the waiter.
                CheckedOut_.push_back(released);
            } else {
                Connections_.push(std::move(conn));
            }
        }
        if (hasWaiter) {
            // Resolve outside the lock: the callback will call TaskReadyThreadSafe
            // which acquires a separate task-queue lock — no re-entrant pool lock.
            waiter.SetValue(TObSession(std::move(conn), Executor_.get(), ShutdownFlag_));
        } else {
            Cv_.notify_one();
        }
        return;
    }

    const bool tenantMemory = lastKind == EObDbErrorKind::TenantMemoryLimit
        || IsTenantMemoryLimitCode(lastCode)
        || LooksLikeTenantMemoryLimit(lastMsg);
    if (tenantMemory) {
        TenantMemoryPressure_.store(true, std::memory_order_relaxed);
        if (!TenantMemoryHintLogged_.exchange(true, std::memory_order_relaxed)) {
            LOG_E(TenantMemoryHint());
        }
        if (!lastMsg.empty()) {
            LOG_E("Dropping OceanBase session after tenant memory error: " << lastMsg);
        } else {
            LOG_E("Dropping OceanBase session after tenant memory error (OB -4013)");
        }
    } else if (!lastMsg.empty()) {
        LOG_W("Dropping non-reusable OceanBase session: " << lastMsg);
    } else {
        LOG_W("Dropping non-reusable OceanBase session and opening a replacement");
    }
    conn->Abandon();
    conn.reset();

    size_t pending = 0;
    {
        std::lock_guard lock(Mutex_);
        if (Shutdown_) {
            return;
        }
        ++PendingReplacements_;
        pending = PendingReplacements_;
    }
    if (!tenantMemory && pending >= MassReplacementThreshold(PoolSize_)) {
        TenantMemoryPressure_.store(true, std::memory_order_relaxed);
        if (!TenantMemoryHintLogged_.exchange(true, std::memory_order_relaxed)) {
            LOG_E("Many OceanBase sessions dropped at once (" << pending
                  << " replacements pending of pool " << PoolSize_
                  << "). If sql audit shows OB -4013 / tenant memory limit, "
                  << TenantMemoryHint());
        }
    }
    Executor_->Submit([this] { ReplaceBrokenConnections(); });
}

void TObConnectionPool::ReplaceBrokenConnections() {
    std::unique_lock reconnectLock(ReconnectMutex_);
    auto backoff = ReconnectInitialBackoff;
    size_t failures = 0;

    for (;;) {
        const bool memoryPressure = TenantMemoryPressure_.load(std::memory_order_relaxed);
        const auto maxBackoff = memoryPressure
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  TenantMemoryReconnectMaxBackoff)
            : ReconnectMaxBackoff;
        const auto pressureInitial = std::chrono::duration_cast<std::chrono::milliseconds>(
            TenantMemoryReconnectInitialBackoff);
        if (memoryPressure && backoff < pressureInitial) {
            backoff = pressureInitial;
        }

        {
            std::lock_guard lock(Mutex_);
            if (Shutdown_ || PendingReplacements_ == 0) {
                return;
            }
        }

        try {
            auto replacement = CreateConnection();
            size_t remaining = 0;
            TPromise<TObSession> waiter;
            bool hasWaiter = false;
            {
                std::lock_guard lock(Mutex_);
                if (Shutdown_) {
                    return;
                }
                if (PendingReplacements_ == 0) {
                    return;
                }
                --PendingReplacements_;
                remaining = PendingReplacements_;
                if (!Waiters_.empty()) {
                    waiter = std::move(Waiters_.front());
                    Waiters_.pop_front();
                    hasWaiter = true;
                    CheckedOut_.push_back(replacement.get());
                } else {
                    Connections_.push(std::move(replacement));
                }
            }
            if (hasWaiter) {
                waiter.SetValue(TObSession(std::move(replacement), Executor_.get(), ShutdownFlag_));
            } else {
                Cv_.notify_one();
            }
            if (failures > 0) {
                LOG_I("Recreated OceanBase connection after " << failures
                      << " failed attempt(s); "
                      << remaining << " replacement(s) still pending");
            }
            failures = 0;
            if (remaining == 0) {
                TenantMemoryPressure_.store(false, std::memory_order_relaxed);
            }
            backoff = memoryPressure && remaining > 0
                ? pressureInitial
                : ReconnectInitialBackoff;
        } catch (const TObDbError& ex) {
            ++failures;
            if (ex.Kind() == EObDbErrorKind::TenantMemoryLimit
                || LooksLikeTenantMemoryLimit(ex.what()))
            {
                TenantMemoryPressure_.store(true, std::memory_order_relaxed);
                if (!TenantMemoryHintLogged_.exchange(true, std::memory_order_relaxed)) {
                    LOG_E(TenantMemoryHint());
                }
            }
            LOG_W("Failed to recreate OceanBase connection (attempt " << failures
                  << ", will retry): " << ex.what());
            std::unique_lock lock(Mutex_);
            if (Shutdown_) {
                return;
            }
            Cv_.wait_for(lock, backoff, [this] { return Shutdown_; });
            if (Shutdown_) {
                return;
            }
            lock.unlock();
            if (backoff < maxBackoff) {
                backoff = std::min(backoff * 2, maxBackoff);
            }
        } catch (const std::exception& ex) {
            ++failures;
            if (LooksLikeTenantMemoryLimit(ex.what())) {
                TenantMemoryPressure_.store(true, std::memory_order_relaxed);
                if (!TenantMemoryHintLogged_.exchange(true, std::memory_order_relaxed)) {
                    LOG_E(TenantMemoryHint());
                }
            }
            LOG_W("Failed to recreate OceanBase connection (attempt " << failures
                  << ", will retry): " << ex.what());
            std::unique_lock lock(Mutex_);
            if (Shutdown_) {
                return;
            }
            Cv_.wait_for(lock, backoff, [this] { return Shutdown_; });
            if (Shutdown_) {
                return;
            }
            lock.unlock();
            if (backoff < maxBackoff) {
                backoff = std::min(backoff * 2, maxBackoff);
            }
        }
    }
}

void TObConnectionPool::CancelAll() {
    ShutdownFlag_->store(true, std::memory_order_release);

    std::vector<TObConnection*> victims;
    std::deque<TPromise<TObSession>> pendingWaiters;
    {
        std::lock_guard lock(Mutex_);
        victims = CheckedOut_;
        pendingWaiters.swap(Waiters_);
    }
    // Wake any coroutines that were parked waiting for a free connection.
    for (auto& waiter : pendingWaiters) {
        waiter.SetException(std::make_exception_ptr(
            TObDbError(1317, "Connection pool cancelled")));
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

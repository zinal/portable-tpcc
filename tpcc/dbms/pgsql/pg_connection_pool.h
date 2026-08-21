#pragma once

#include "pg_session.h"
#include <thread_pool.h>

#include <pqxx/pqxx>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

namespace NTpcc {

class PgConnectionPool {
public:
    PgConnectionPool(const std::string& connectionString,
                     size_t poolSize,
                     size_t ioThreads,
                     const std::string& path = {});

    ~PgConnectionPool();

    // Acquires a PgSession from the pool. Blocks if none available.
    PgSession AcquireSession();

    // Non-blocking acquire. Empty optional when the pool has no free session.
    std::optional<PgSession> TryAcquireSession();

    // Returns a connection back to the pool.
    void ReleaseSession(PgSession session);

    // Convenience RAII wrapper
    class SessionGuard {
    public:
        SessionGuard(PgConnectionPool& pool, PgSession session)
            : pool_(pool), session_(std::move(session)) {}

        ~SessionGuard() {
            if (session_.HasConnection()) {
                pool_.ReleaseSession(std::move(session_));
            }
        }

        SessionGuard(SessionGuard&& o) noexcept
            : pool_(o.pool_), session_(std::move(o.session_)) {}

        SessionGuard(const SessionGuard&) = delete;
        SessionGuard& operator=(const SessionGuard&) = delete;
        SessionGuard& operator=(SessionGuard&&) = delete;

        PgSession& operator*() { return session_; }
        PgSession* operator->() { return &session_; }

    private:
        PgConnectionPool& pool_;
        PgSession session_;
    };

    SessionGuard AcquireGuard();

    std::optional<SessionGuard> TryAcquireGuard();

    // Cancel all in-flight queries on checked-out connections.
    // Call before Join/destruction to unblock threads waiting on locks.
    void CancelAll();

    IExecutor* GetExecutor() { return executor_.get(); }
    size_t GetPoolSize() const { return poolSize_; }

private:
    std::unique_ptr<pqxx::connection> CreateConnection() const;

    std::string connectionString_;
    std::string path_;
    size_t poolSize_;
    std::unique_ptr<TThreadPool> executor_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::unique_ptr<pqxx::connection>> connections_;
    std::vector<pqxx::connection*> checkedOut_;
    std::shared_ptr<std::atomic<bool>> sessionShutdownFlag_ =
        std::make_shared<std::atomic<bool>>(false);
    bool shutdown_ = false;
};

} // namespace NTpcc

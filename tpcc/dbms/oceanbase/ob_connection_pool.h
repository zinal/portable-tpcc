#pragma once

#include "ob_connection.h"
#include "ob_session.h"

#include <thread_pool.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace NTpcc {

class TObConnectionPool {
public:
    TObConnectionPool(
        const std::string& connectionString,
        size_t poolSize,
        size_t ioThreads,
        const std::string& path = {});

    TObConnectionPool(TObConnectionConfig config, size_t poolSize, size_t ioThreads);

    ~TObConnectionPool();

    TObConnectionPool(const TObConnectionPool&) = delete;
    TObConnectionPool& operator=(const TObConnectionPool&) = delete;

    TObSession AcquireSession();
    void ReleaseSession(TObSession session);

    class TSessionGuard {
    public:
        TSessionGuard(TObConnectionPool& pool, TObSession session)
            : Pool_(&pool)
            , Session_(std::move(session))
        {}

        ~TSessionGuard() {
            if (Pool_ && Session_.HasConnection()) {
                Pool_->ReleaseSession(std::move(Session_));
            }
        }

        TSessionGuard(TSessionGuard&& o) noexcept
            : Pool_(o.Pool_)
            , Session_(std::move(o.Session_))
        {
            o.Pool_ = nullptr;
        }

        TSessionGuard(const TSessionGuard&) = delete;
        TSessionGuard& operator=(const TSessionGuard&) = delete;
        TSessionGuard& operator=(TSessionGuard&&) = delete;

        TObSession& operator*() {
            return Session_;
        }

        TObSession* operator->() {
            return &Session_;
        }

    private:
        TObConnectionPool* Pool_;
        TObSession Session_;
    };

    TSessionGuard AcquireGuard();
    void CancelAll();

    IExecutor* GetExecutor() {
        return Executor_.get();
    }

    size_t GetPoolSize() const {
        return PoolSize_;
    }

private:
    std::unique_ptr<TObConnection> CreateConnection() const;

    TObConnectionConfig Config_;
    size_t PoolSize_ = 0;
    std::unique_ptr<TThreadPool> Executor_;
    std::mutex Mutex_;
    std::condition_variable Cv_;
    std::queue<std::unique_ptr<TObConnection>> Connections_;
    std::vector<TObConnection*> CheckedOut_;
    std::shared_ptr<std::atomic<bool>> ShutdownFlag_ =
        std::make_shared<std::atomic<bool>>(false);
    bool Shutdown_ = false;
};

} // namespace NTpcc

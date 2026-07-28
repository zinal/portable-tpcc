#pragma once

#include <atomic>
#include <thread>

namespace NTpcc {

class TSpinLockBase {
protected:
    TSpinLockBase() = default;

    TSpinLockBase(const TSpinLockBase& other)
        : Val_(other.Val_.load(std::memory_order_acquire))
    {
    }

    TSpinLockBase& operator=(const TSpinLockBase& other) {
        Val_.store(other.Val_.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }

public:
    inline bool IsLocked() const noexcept {
        return Val_.load(std::memory_order_acquire);
    }

    inline bool TryAcquire() noexcept {
        return !Val_.exchange(1, std::memory_order_acquire);
    }

    inline void Release() noexcept {
        Val_.store(0, std::memory_order_release);
    }

    inline bool try_lock() noexcept {
        return TryAcquire();
    }

    inline void unlock() noexcept {
        Release();
    }

protected:
    std::atomic<bool> Val_{false};
};

static inline void SpinLockPause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

class TSpinLock: public TSpinLockBase {
public:
    using TSpinLockBase::TSpinLockBase;

    inline void Acquire() noexcept {
        for (;;) {
            if (TryAcquire()) {
                return;
            }
            do {
                SpinLockPause();
            } while (IsLocked());
        }
    }

    inline void lock() noexcept {
        Acquire();
    }

    class TGuard {
    public:
        explicit TGuard(TSpinLock& lock) noexcept
            : Lock_(lock)
        {
            Lock_.lock();
        }

        ~TGuard() noexcept {
            Lock_.unlock();
        }

        TGuard(const TGuard&) = delete;
        TGuard& operator=(const TGuard&) = delete;

    private:
        TSpinLock& Lock_;
    };
};

} // namespace NTpcc

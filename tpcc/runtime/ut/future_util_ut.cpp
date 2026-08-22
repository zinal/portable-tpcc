#include <gtest/gtest.h>

#include <coro_traits.h>
#include <future_util.h>
#include <task_queue.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace NTpcc;
using namespace std::chrono_literals;

TEST(FutureUtil, MakeReadyFutureValueAndVoid) {
    auto ready = MakeReadyFuture(7);
    EXPECT_TRUE(ready.IsReady());
    EXPECT_EQ(ready.Get(), 7);

    auto readyVoid = MakeReadyFuture();
    EXPECT_TRUE(readyVoid.IsReady());
    readyVoid.Get();
}

TEST(FutureUtil, ThenDoesNotBlockOnIncompleteSource) {
    TPromise<int> promise;
    auto src = promise.GetFuture();

    std::atomic<bool> continuationRan{false};
    auto out = Then(std::move(src), [&](int v) {
        continuationRan.store(true);
        return v + 1;
    });

    EXPECT_FALSE(out.IsReady());
    EXPECT_FALSE(continuationRan.load());

    promise.SetValue(41);
    EXPECT_TRUE(out.IsReady());
    EXPECT_TRUE(continuationRan.load());
    EXPECT_EQ(out.Get(), 42);
}

TEST(FutureUtil, ThenVoidSourceToValue) {
    TPromise<void> promise;
    auto src = promise.GetFuture();
    auto out = Then(std::move(src), []() { return std::string("ok"); });
    EXPECT_FALSE(out.IsReady());
    promise.SetValue();
    EXPECT_EQ(out.Get(), "ok");
}

TEST(FutureUtil, ThenFlattensNestedFuture) {
    TPromise<int> inner;
    auto out = Then(MakeReadyFuture(1), [&](int v) {
        return Then(inner.GetFuture(), [v](int w) { return v + w; });
    });
    EXPECT_FALSE(out.IsReady());
    inner.SetValue(2);
    EXPECT_EQ(out.Get(), 3);
}

TEST(FutureUtil, ThenPropagatesException) {
    TPromise<int> promise;
    auto out = Then(promise.GetFuture(), [](int v) { return v; });
    promise.SetException(std::make_exception_ptr(std::runtime_error("boom")));
    EXPECT_THROW(out.Get(), std::runtime_error);
}

TEST(FutureUtil, CatchToValueMapsStdException) {
    TPromise<int> promise;
    auto out = CatchToValue(promise.GetFuture(), [](const std::exception& ex) {
        EXPECT_STREQ(ex.what(), "boom");
        return 99;
    });
    promise.SetException(std::make_exception_ptr(std::runtime_error("boom")));
    EXPECT_EQ(out.Get(), 99);
}

TEST(FutureUtil, ThenFoldSequencesItems) {
    auto out = ThenFold(
        std::vector<int>{1, 2, 3},
        0,
        [](int acc, int item) { return MakeReadyFuture(acc + item); });
    EXPECT_EQ(out.Get(), 6);
}

namespace {

struct TBatchLike {
    bool Ok = true;
    int Sum = 0;
};

} // namespace

TEST(FutureUtil, ThenFoldCanSkipRemainingWork) {
    std::vector<int> visited;
    auto out = ThenFold(
        std::vector<int>{1, 2, 3, 4},
        TBatchLike{},
        [&](TBatchLike acc, int item) {
            if (!acc.Ok) {
                return MakeReadyFuture(std::move(acc));
            }
            visited.push_back(item);
            acc.Sum += item;
            if (item == 2) {
                acc.Ok = false;
            }
            return MakeReadyFuture(std::move(acc));
        });
    auto result = out.Get();
    EXPECT_FALSE(result.Ok);
    EXPECT_EQ(result.Sum, 3);
    // Remaining items still see the failed acc and must not run work.
    EXPECT_EQ(visited, (std::vector<int>{1, 2}));
}

namespace {

void UpdatePeak(std::atomic<int>& current, std::atomic<int>& peak) {
    const int now = current.fetch_add(1, std::memory_order_relaxed) + 1;
    int old = peak.load(std::memory_order_relaxed);
    while (now > old && !peak.compare_exchange_weak(old, now, std::memory_order_relaxed)) {
    }
}

TFuture<int> ParkOnScheduler(
    ITaskQueue& taskQueue,
    size_t threadHint,
    TFuture<int> src,
    std::atomic<int>& current,
    std::atomic<int>& peak)
{
    // Started on the test thread; TTaskReady must publish the handle without
    // racing the scheduler that may resume this frame immediately.
    co_await TTaskReady(taskQueue, threadHint);
    UpdatePeak(current, peak);
    auto chained = Then(std::move(src), [](int v) { return v + 1; });
    const int value = co_await TSuspendWithFuture(std::move(chained), taskQueue, threadHint);
    current.fetch_sub(1, std::memory_order_relaxed);
    co_return value;
}

} // namespace

TEST(FutureUtil, IncompleteThenLetsInflightExceedSchedulerThreads) {
    constexpr int kThreads = 2;
    constexpr int kWorkers = 8;

    auto taskQueue = CreateTaskQueue(kThreads, 100, 256, 256);
    taskQueue->Run();

    std::vector<TPromise<int>> promises(kWorkers);
    std::vector<TFuture<int>> done;
    done.reserve(kWorkers);
    std::atomic<int> current{0};
    std::atomic<int> peak{0};

    for (int i = 0; i < kWorkers; ++i) {
        done.push_back(ParkOnScheduler(
            *taskQueue,
            static_cast<size_t>(i),
            promises[static_cast<size_t>(i)].GetFuture(),
            current,
            peak));
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (peak.load(std::memory_order_relaxed) < kWorkers
           && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_GT(peak.load(), kThreads);
    EXPECT_EQ(peak.load(), kWorkers);

    for (auto& promise : promises) {
        promise.SetValue(1);
    }
    for (auto& future : done) {
        EXPECT_EQ(future.Get(), 2);
    }

    taskQueue->WakeupAndNeverSleep();
    taskQueue->Join();
}

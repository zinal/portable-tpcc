#pragma once

#include <future_util.h>
#include <thread_pool.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

namespace NTpcc {

struct TDummyDelayConfig {
    int64_t MinUs = 0;
    int64_t MaxUs = 0;
};

// Parse a dummy connection string of whitespace-separated key=value pairs.
// Accepted keys: delay_us_min, delay_us_max. Empty string → both zero.
TDummyDelayConfig ParseDummyConnection(const std::string& connection);

void ValidateDummyDelayConfig(const TDummyDelayConfig& config);

// Inclusive uniform sample. Independent of the TPC-C workload RNG.
std::chrono::microseconds SampleDummyDelay(const TDummyDelayConfig& config);

template <typename T>
TFuture<T> CompleteAfterDelay(IExecutor* executor, std::chrono::microseconds delay, T value) {
    if (delay.count() <= 0 || executor == nullptr) {
        return MakeReadyFuture(std::move(value));
    }
    TPromise<T> promise;
    auto future = promise.GetFuture();
    executor->Submit([promise, delay, value = std::move(value)]() mutable {
        std::this_thread::sleep_for(delay);
        promise.SetValue(std::move(value));
    });
    return future;
}

} // namespace NTpcc

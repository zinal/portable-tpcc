#pragma once

#include <future.h>

#include <library/cpp/threading/future/future.h>

namespace NTpcc {

// Bridge an NYdb / NThreading future onto NTpcc::TFuture without waiting on
// the calling thread. The Subscribe callback runs on the SDK completion
// thread; GetValueSync there is non-blocking because the source is already
// ready. Task-queue coroutines MUST still resume via TSuspendWithFuture.
template <typename T>
TFuture<T> BridgeYdbFuture(NThreading::TFuture<T> src) {
    TPromise<T> promise;
    auto future = promise.GetFuture();
    src.Subscribe([promise = std::move(promise)](const NThreading::TFuture<T>& ready) mutable {
        try {
            promise.SetValue(ready.GetValueSync());
        } catch (...) {
            promise.SetException(std::current_exception());
        }
    });
    return future;
}

} // namespace NTpcc

#pragma once

#include <future.h>

#include <exception>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace NTpcc {

template <typename T>
TFuture<T> MakeReadyFuture(T value) {
    TPromise<T> promise;
    auto future = promise.GetFuture();
    promise.SetValue(std::move(value));
    return future;
}

inline TFuture<void> MakeReadyFuture() {
    TPromise<void> promise;
    auto future = promise.GetFuture();
    promise.SetValue();
    return future;
}

namespace NFutureUtilDetail {

template <typename T>
struct TUnwrapFuture {
    static constexpr bool IsFuture = false;
    using TType = T;
};

template <typename T>
struct TUnwrapFuture<TFuture<T>> {
    static constexpr bool IsFuture = true;
    using TType = T;
};

template <typename U>
void FulfillFromFuture(TPromise<U>& promise, TFuture<U>& src) {
    if constexpr (std::is_void_v<U>) {
        src.Get();
        promise.SetValue();
    } else {
        promise.SetValue(src.Get());
    }
}

template <typename U, typename Fn>
void InvokeAndFulfill(TPromise<U>& promise, Fn&& fn) {
    if constexpr (std::is_void_v<U>) {
        std::forward<Fn>(fn)();
        promise.SetValue();
    } else {
        promise.SetValue(std::forward<Fn>(fn)());
    }
}

template <typename U, typename SrcFuture>
void FlattenInto(TPromise<U> promise, SrcFuture src) {
    src.Subscribe([src, promise = std::move(promise)]() mutable {
        try {
            FulfillFromFuture(promise, src);
        } catch (...) {
            promise.SetException(std::current_exception());
        }
    });
}

} // namespace NFutureUtilDetail

// Chain `src` through `fn` without blocking the caller.
// `fn` is invoked when `src` is ready (often on an IO / executor thread).
// If `fn` returns TFuture<U>, the result is flattened into TFuture<U>.
template <typename T, typename F>
auto Then(TFuture<T> src, F fn) {
    if constexpr (std::is_void_v<T>) {
        using TRaw = std::decay_t<std::invoke_result_t<F>>;
        using TUnwrapped = typename NFutureUtilDetail::TUnwrapFuture<TRaw>::TType;
        TPromise<TUnwrapped> promise;
        auto future = promise.GetFuture();
        src.Subscribe([src, promise = std::move(promise), fn = std::move(fn)]() mutable {
            try {
                src.Get();
                if constexpr (NFutureUtilDetail::TUnwrapFuture<TRaw>::IsFuture) {
                    auto next = fn();
                    NFutureUtilDetail::FlattenInto(std::move(promise), std::move(next));
                } else {
                    NFutureUtilDetail::InvokeAndFulfill(promise, fn);
                }
            } catch (...) {
                promise.SetException(std::current_exception());
            }
        });
        return future;
    } else {
        using TRaw = std::decay_t<std::invoke_result_t<F, T>>;
        using TUnwrapped = typename NFutureUtilDetail::TUnwrapFuture<TRaw>::TType;
        TPromise<TUnwrapped> promise;
        auto future = promise.GetFuture();
        src.Subscribe([src, promise = std::move(promise), fn = std::move(fn)]() mutable {
            try {
                auto value = src.Get();
                if constexpr (NFutureUtilDetail::TUnwrapFuture<TRaw>::IsFuture) {
                    auto next = fn(std::move(value));
                    NFutureUtilDetail::FlattenInto(std::move(promise), std::move(next));
                } else {
                    NFutureUtilDetail::InvokeAndFulfill(promise, [&]() {
                        return fn(std::move(value));
                    });
                }
            } catch (...) {
                promise.SetException(std::current_exception());
            }
        });
        return future;
    }
}

// Convert a failed `src` into a value via `handler(const std::exception&)`.
// Non-std::exception failures are propagated as exceptions.
template <typename T, typename H>
TFuture<T> CatchToValue(TFuture<T> src, H handler) {
    static_assert(!std::is_void_v<T>, "CatchToValue requires a non-void future");
    TPromise<T> promise;
    auto future = promise.GetFuture();
    src.Subscribe([src, promise = std::move(promise), handler = std::move(handler)]() mutable {
        try {
            promise.SetValue(src.Get());
        } catch (const std::exception& ex) {
            try {
                promise.SetValue(handler(ex));
            } catch (...) {
                promise.SetException(std::current_exception());
            }
        } catch (...) {
            promise.SetException(std::current_exception());
        }
    });
    return future;
}

// Sequential left fold. `step(acc, item)` MUST return TFuture<Acc>.
template <typename Item, typename Acc, typename Step>
TFuture<Acc> ThenFold(std::vector<Item> items, Acc acc, Step step) {
    struct TRunner : std::enable_shared_from_this<TRunner> {
        std::vector<Item> Items;
        Acc Acc_;
        Step Step_;
        size_t Index = 0;

        TRunner(std::vector<Item> items, Acc acc, Step step)
            : Items(std::move(items))
            , Acc_(std::move(acc))
            , Step_(std::move(step))
        {}

        TFuture<Acc> Run() {
            if (Index >= Items.size()) {
                return MakeReadyFuture(std::move(Acc_));
            }
            auto self = this->shared_from_this();
            return Then(
                Step_(std::move(Acc_), Items[Index]),
                [self](Acc next) {
                    self->Acc_ = std::move(next);
                    ++self->Index;
                    return self->Run();
                });
        }
    };

    auto runner = std::make_shared<TRunner>(std::move(items), std::move(acc), std::move(step));
    return runner->Run();
}

} // namespace NTpcc

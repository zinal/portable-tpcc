#include <gtest/gtest.h>

#include <future_util.h>
#include <ydb_future.h>

#include <library/cpp/threading/future/future.h>

#include <stdexcept>
#include <string>

using namespace NTpcc;

TEST(YdbFutureBridge, IncompleteSourceDoesNotBlockCaller) {
    auto promise = NThreading::NewPromise<int>();
    auto out = BridgeYdbFuture(promise.GetFuture());
    EXPECT_FALSE(out.IsReady());
    promise.SetValue(7);
    EXPECT_TRUE(out.IsReady());
    EXPECT_EQ(out.Get(), 7);
}

TEST(YdbFutureBridge, ReadySourceCompletes) {
    auto out = BridgeYdbFuture(NThreading::MakeFuture(42));
    EXPECT_EQ(out.Get(), 42);
}

TEST(YdbFutureBridge, PropagatesException) {
    auto promise = NThreading::NewPromise<int>();
    auto out = BridgeYdbFuture(promise.GetFuture());
    promise.SetException(std::make_exception_ptr(std::runtime_error("boom")));
    EXPECT_THROW(out.Get(), std::runtime_error);
}

TEST(YdbFutureBridge, ThenChainsWithoutWaitingOnIncompleteSource) {
    auto promise = NThreading::NewPromise<int>();
    bool ran = false;
    auto out = Then(
        BridgeYdbFuture(promise.GetFuture()),
        [&](int v) {
            ran = true;
            return v + 1;
        });
    EXPECT_FALSE(out.IsReady());
    EXPECT_FALSE(ran);
    promise.SetValue(41);
    EXPECT_TRUE(out.IsReady());
    EXPECT_TRUE(ran);
    EXPECT_EQ(out.Get(), 42);
}

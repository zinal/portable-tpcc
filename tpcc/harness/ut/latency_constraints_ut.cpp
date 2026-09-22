#include <gtest/gtest.h>

#include <run_loop.h>

#include <chrono>
#include <string>
#include <vector>

using namespace NTpcc;

namespace {

TTerminalStats::TLatencySample FullLatency(std::chrono::microseconds full) {
    TTerminalStats::TLatencySample sample;
    sample.Full = full;
    return sample;
}

} // namespace

TEST(LatencyConstraints, FastNewOrderPasses) {
    TTerminalStats stats;
    stats.AddOK(ETransactionType::NewOrder, FullLatency(std::chrono::milliseconds(20)));
    std::vector<TLatencyConstraintViolation> out;
    CollectLatencyConstraintViolations(stats, "ms", out);
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(FormatInvalidRunLatencyBanner(out).empty());
}

TEST(LatencyConstraints, SlowNewOrderFails) {
    TTerminalStats stats;
    const auto sample = FullLatency(std::chrono::seconds(12));
    for (int i = 0; i < 10; ++i) {
        stats.AddOK(ETransactionType::NewOrder, sample);
    }
    std::vector<TLatencyConstraintViolation> out;
    CollectLatencyConstraintViolations(stats, "ms", out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].TypeName, "NewOrder");
    EXPECT_GE(out[0].P90Ms, TPCC_P90_LIMIT_MS);
    EXPECT_EQ(out[0].LimitMs, TPCC_P90_LIMIT_MS);
    const auto banner = FormatInvalidRunLatencyBanner(out);
    EXPECT_NE(banner.find("INVALID RUN"), std::string::npos);
    EXPECT_NE(banner.find("NewOrder"), std::string::npos);
    EXPECT_NE(banner.find("Clause 5.2.5.3"), std::string::npos);
}

TEST(LatencyConstraints, ExactFiveSecondsFails) {
    TTerminalStats stats;
    stats.AddOK(ETransactionType::Payment, FullLatency(std::chrono::seconds(5)));
    std::vector<TLatencyConstraintViolation> out;
    CollectLatencyConstraintViolations(stats, "ms", out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].TypeName, "Payment");
}

TEST(LatencyConstraints, StockLevelTwentySeconds) {
    TTerminalStats stats;
    stats.AddOK(ETransactionType::StockLevel, FullLatency(std::chrono::seconds(15)));
    std::vector<TLatencyConstraintViolation> out;
    CollectLatencyConstraintViolations(stats, "ms", out);
    EXPECT_TRUE(out.empty());

    TTerminalStats slow;
    slow.AddOK(ETransactionType::StockLevel, FullLatency(std::chrono::seconds(21)));
    CollectLatencyConstraintViolations(slow, "ms", out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].TypeName, "StockLevel");
    EXPECT_EQ(out[0].LimitMs, TPCC_STOCK_LEVEL_P90_LIMIT_MS);
}

TEST(LatencyConstraints, UsUnitConverts) {
    TTerminalStats stats(4096, 10'000'000ull, /*recordMicroseconds=*/true);
    stats.AddOK(ETransactionType::Payment, FullLatency(std::chrono::seconds(6)));
    std::vector<TLatencyConstraintViolation> out;
    CollectLatencyConstraintViolations(stats, "us", out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_STREQ(out[0].TypeName, "Payment");
    EXPECT_GE(out[0].P90Ms, TPCC_P90_LIMIT_MS);
}

TEST(LatencyConstraints, FailedOnlyDoesNotCount) {
    TTerminalStats stats;
    stats.IncFailed(ETransactionType::NewOrder);
    std::vector<TLatencyConstraintViolation> out;
    CollectLatencyConstraintViolations(stats, "ms", out);
    EXPECT_TRUE(out.empty());
}

TEST(LatencyConstraints, PercentileToMilliseconds) {
    EXPECT_EQ(PercentileToMilliseconds(5000, "ms"), 5000u);
    EXPECT_EQ(PercentileToMilliseconds(5'000'000, "us"), 5000u);
    EXPECT_EQ(PercentileToMilliseconds(4999, nullptr), 4999u);
}

TEST(LatencyConstraints, TpccP90Limits) {
    EXPECT_EQ(TpccP90LimitMs(ETransactionType::NewOrder), TPCC_P90_LIMIT_MS);
    EXPECT_EQ(TpccP90LimitMs(ETransactionType::Delivery), TPCC_P90_LIMIT_MS);
    EXPECT_EQ(TpccP90LimitMs(ETransactionType::OrderStatus), TPCC_P90_LIMIT_MS);
    EXPECT_EQ(TpccP90LimitMs(ETransactionType::Payment), TPCC_P90_LIMIT_MS);
    EXPECT_EQ(TpccP90LimitMs(ETransactionType::StockLevel), TPCC_STOCK_LEVEL_P90_LIMIT_MS);
}

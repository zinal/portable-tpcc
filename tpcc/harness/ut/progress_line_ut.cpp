#include <gtest/gtest.h>

#include <run_loop.h>

#include <array>
#include <chrono>
#include <string>

using namespace NTpcc;

namespace {

std::array<size_t, TRANSACTION_TYPE_COUNT> CountsWith(ETransactionType type, size_t n) {
    std::array<size_t, TRANSACTION_TYPE_COUNT> counts{};
    counts[static_cast<size_t>(type)] = n;
    return counts;
}

} // namespace

TEST(ProgressLine, WarmupSamplesStayOutOfMeasurementHistograms) {
    TTerminalStats stats;
    stats.AddProgressOK(ETransactionType::NewOrder, std::chrono::milliseconds(10));
    stats.AddProgressOK(ETransactionType::NewOrder, std::chrono::milliseconds(30));
    stats.AddProgressUserAborted(ETransactionType::NewOrder, std::chrono::milliseconds(40));
    stats.AddProgressOK(ETransactionType::Payment, std::chrono::milliseconds(20));

    const auto& live = stats.GetStats(ETransactionType::NewOrder);
    EXPECT_EQ(live.ProgressOK.load(), 2u);
    EXPECT_EQ(live.ProgressUserAborted.load(), 1u);
    EXPECT_EQ(live.OK.load(), 0u);
    EXPECT_EQ(live.LatencyHistogramFullMs.TotalCount(), 0u);
    EXPECT_EQ(live.ProgressLatencyFullMs.TotalCount(), 3u);

    TTerminalStats aggregated;
    stats.Collect(aggregated);
    EXPECT_EQ(aggregated.GetStats(ETransactionType::NewOrder).ProgressLatencyFullMs.TotalCount(), 0u);
    EXPECT_EQ(aggregated.GetStats(ETransactionType::NewOrder).OK.load(), 0u);

    stats.CollectProgressLatency(aggregated);
    EXPECT_EQ(aggregated.GetStats(ETransactionType::NewOrder).ProgressLatencyFullMs.TotalCount(), 3u);
    EXPECT_EQ(aggregated.GetStats(ETransactionType::Payment).ProgressLatencyFullMs.TotalCount(), 1u);
}

TEST(ProgressLine, CountsAreIntervalDeltasAndEveryTypeShowsP90) {
    TTerminalStats stats;
    stats.AddProgressOK(ETransactionType::NewOrder, std::chrono::milliseconds(10));
    stats.AddProgressOK(ETransactionType::NewOrder, std::chrono::milliseconds(30));
    stats.AddProgressUserAborted(ETransactionType::NewOrder, std::chrono::milliseconds(40));
    stats.AddProgressOK(ETransactionType::Payment, std::chrono::milliseconds(20));

    TTerminalStats aggregated;
    stats.Collect(aggregated);
    stats.CollectProgressLatency(aggregated);

    std::array<size_t, TRANSACTION_TYPE_COUNT> cumulative{};
    cumulative[static_cast<size_t>(ETransactionType::NewOrder)] = 3;
    cumulative[static_cast<size_t>(ETransactionType::Payment)] = 1;
    std::array<size_t, TRANSACTION_TYPE_COUNT> last{};

    const auto first = FormatProgressTransactionFields(aggregated, cumulative, last, "ms");
    const auto newOrderP90 = aggregated.GetStats(ETransactionType::NewOrder)
        .ProgressLatencyFullMs.GetValueAtPercentile(90);
    const auto paymentP90 = aggregated.GetStats(ETransactionType::Payment)
        .ProgressLatencyFullMs.GetValueAtPercentile(90);

    EXPECT_NE(first.find("  NewOrder:3(p90=" + std::to_string(newOrderP90) + "ms)"), std::string::npos);
    EXPECT_NE(first.find("  Payment:1(p90=" + std::to_string(paymentP90) + "ms)"), std::string::npos);
    EXPECT_EQ(first.find("Delivery"), std::string::npos);
    EXPECT_EQ(first.find("p50"), std::string::npos);
    EXPECT_EQ(first.find("p99"), std::string::npos);
    EXPECT_EQ(first.find("OK"), std::string::npos);

    cumulative[static_cast<size_t>(ETransactionType::NewOrder)] = 5;
    const auto second = FormatProgressTransactionFields(aggregated, cumulative, last, "ms");
    EXPECT_NE(second.find("  NewOrder:2(p90=" + std::to_string(newOrderP90) + "ms)"), std::string::npos);
    EXPECT_NE(second.find("  Payment:0(p90=" + std::to_string(paymentP90) + "ms)"), std::string::npos);
}

TEST(ProgressLine, MeasurementP90ReplacesWarmupP90) {
    TTerminalStats stats;
    stats.AddProgressOK(ETransactionType::NewOrder, std::chrono::milliseconds(10));
    stats.AddOK(ETransactionType::NewOrder, [] {
        TTerminalStats::TLatencySample sample;
        sample.Full = std::chrono::milliseconds(100);
        return sample;
    }());

    TTerminalStats aggregated;
    stats.Collect(aggregated);
    stats.CollectProgressLatency(aggregated);

    const auto& row = aggregated.GetStats(ETransactionType::NewOrder);
    ASSERT_GT(row.LatencyHistogramFullMs.TotalCount(), 0u);
    const auto measuredP90 = row.LatencyHistogramFullMs.GetValueAtPercentile(90);
    const auto warmupP90 = row.ProgressLatencyFullMs.GetValueAtPercentile(90);
    ASSERT_NE(measuredP90, warmupP90);

    auto cumulative = CountsWith(ETransactionType::NewOrder, 1);
    std::array<size_t, TRANSACTION_TYPE_COUNT> last{};
    const auto line = FormatProgressTransactionFields(aggregated, cumulative, last, "ms");
    EXPECT_NE(line.find("(p90=" + std::to_string(measuredP90) + "ms)"), std::string::npos);
    EXPECT_EQ(line.find("(p90=" + std::to_string(warmupP90) + "ms)"), std::string::npos);
}

TEST(ProgressLine, ClearProgressOnceDropsWarmupLatencyOnly) {
    TTerminalStats stats;
    stats.AddProgressOK(ETransactionType::NewOrder, std::chrono::milliseconds(10));
    stats.AddOK(ETransactionType::NewOrder, [] {
        TTerminalStats::TLatencySample sample;
        sample.Full = std::chrono::milliseconds(20);
        return sample;
    }());

    EXPECT_TRUE(stats.ClearProgressOnce());
    EXPECT_FALSE(stats.ClearProgressOnce());

    const auto& row = stats.GetStats(ETransactionType::NewOrder);
    EXPECT_EQ(row.ProgressOK.load(), 0u);
    EXPECT_EQ(row.ProgressLatencyFullMs.TotalCount(), 0u);
    EXPECT_EQ(row.OK.load(), 1u);
    EXPECT_EQ(row.LatencyHistogramFullMs.TotalCount(), 1u);
}

#include <gtest/gtest.h>

#include <workload_config.h>

using namespace NTpcc;

TEST(WorkloadConfig, DefaultsMatchConstants) {
    const auto w = MakeDefaultWorkloadConfig();
    EXPECT_EQ(w.TerminalsPerWarehouse, TERMINALS_PER_WAREHOUSE);
    EXPECT_DOUBLE_EQ(
        w.PerTx[static_cast<size_t>(ETransactionType::NewOrder)].Weight, NEW_ORDER_WEIGHT);
    EXPECT_EQ(
        w.PerTx[static_cast<size_t>(ETransactionType::NewOrder)].KeyingTimeMs,
        NEW_ORDER_KEYING_TIME.count() * 1000);
    EXPECT_EQ(
        w.PerTx[static_cast<size_t>(ETransactionType::Payment)].ThinkTimeMs,
        PAYMENT_THINK_TIME.count() * 1000);
}

TEST(HistogramConfig, MapsLinearExpParams) {
    THistogramConfig h;
    EXPECT_EQ(h.HdrTill(), 4096u);
    EXPECT_EQ(h.MaxValue(), 32768u);

    h.Configured = true;
    h.Unit = "us";
    h.Highest = 120000000;
    EXPECT_EQ(h.HdrTill(), 4096u);
    EXPECT_EQ(h.MaxValue(), 120000000u);

    h.Highest = 100;
    EXPECT_EQ(h.HdrTill(), 100u);
}

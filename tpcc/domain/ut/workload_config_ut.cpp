#include <gtest/gtest.h>

#include <stdexcept>

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
    EXPECT_EQ(w.NewOrderRemoteWarehousePercent, NEW_ORDER_REMOTE_WAREHOUSE_PERCENT);
    EXPECT_EQ(w.PaymentRemoteWarehousePercent, PAYMENT_REMOTE_WAREHOUSE_PERCENT);
    EXPECT_EQ(w.NewOrderMaxRemoteWarehouses, NEW_ORDER_MAX_REMOTE_WAREHOUSES);
}

TEST(WorkloadConfig, RemoteWarehousePercentRange) {
    EXPECT_NO_THROW(ValidateRemoteWarehousePercent(0, "x"));
    EXPECT_NO_THROW(ValidateRemoteWarehousePercent(100, "x"));
    EXPECT_THROW(ValidateRemoteWarehousePercent(-1, "x"), std::runtime_error);
    EXPECT_THROW(ValidateRemoteWarehousePercent(101, "x"), std::runtime_error);
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

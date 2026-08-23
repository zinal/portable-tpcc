#include <gtest/gtest.h>

#include <load_batch.h>

using namespace NTpcc;

TEST(ObLoadBatchRows, DefaultsWhenNonPositive) {
    EXPECT_EQ(EffectiveObLoadBatchRows(10, 0), static_cast<size_t>(DEFAULT_LOAD_BATCH_ROWS));
    EXPECT_EQ(EffectiveObLoadBatchRows(10, -1), static_cast<size_t>(DEFAULT_LOAD_BATCH_ROWS));
    EXPECT_EQ(DEFAULT_LOAD_BATCH_ROWS, 2000);
}

TEST(ObLoadBatchRows, HonorsPositiveOverride) {
    EXPECT_EQ(EffectiveObLoadBatchRows(3, 1), 1u);
    EXPECT_EQ(EffectiveObLoadBatchRows(3, 2000), 2000u);
    EXPECT_EQ(EffectiveObLoadBatchRows(3, 10000), 10000u);
}

TEST(ObLoadBatchRows, CapsAtPreparedPlaceholderLimit) {
    // 65535 / 10 = 6553 rows, so 10000 is reduced.
    EXPECT_EQ(EffectiveObLoadBatchRows(10, 10000), 6553u);
    EXPECT_EQ(EffectiveObLoadBatchRows(0, 2000), 1u);
}

#include <gtest/gtest.h>

#include <load_batch.h>

using namespace NTpcc;

TEST(PgLoadBatchRows, DefaultsWhenNonPositive) {
    EXPECT_EQ(EffectiveLoadBatchRows(0), DEFAULT_LOAD_BATCH_ROWS);
    EXPECT_EQ(EffectiveLoadBatchRows(-1), DEFAULT_LOAD_BATCH_ROWS);
    EXPECT_EQ(DEFAULT_LOAD_BATCH_ROWS, 2000);
}

TEST(PgLoadBatchRows, HonorsPositiveOverride) {
    EXPECT_EQ(EffectiveLoadBatchRows(1), 1);
    EXPECT_EQ(EffectiveLoadBatchRows(2000), 2000);
    EXPECT_EQ(EffectiveLoadBatchRows(10000), 10000);
}

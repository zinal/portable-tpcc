#include <gtest/gtest.h>

#include <load_batch.h>

using namespace NTpcc;

TEST(YdbLoadBatchRows, DefaultsWhenNonPositive) {
    EXPECT_EQ(EffectiveYdbLoadBatchRows(0), DEFAULT_YDB_LOAD_BATCH_ROWS);
    EXPECT_EQ(EffectiveYdbLoadBatchRows(-1), DEFAULT_YDB_LOAD_BATCH_ROWS);
    EXPECT_EQ(DEFAULT_YDB_LOAD_BATCH_ROWS, DEFAULT_LOAD_BATCH_ROWS);
    EXPECT_EQ(DEFAULT_LOAD_BATCH_ROWS, 2000);
}

TEST(YdbLoadBatchRows, HonorsPositiveOverride) {
    EXPECT_EQ(EffectiveYdbLoadBatchRows(1), 1);
    EXPECT_EQ(EffectiveYdbLoadBatchRows(2000), 2000);
    EXPECT_EQ(EffectiveYdbLoadBatchRows(10000), 10000);
    EXPECT_EQ(EffectiveYdbLoadBatchRows(50000), 50000);
}

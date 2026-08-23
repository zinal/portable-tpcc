#include <gtest/gtest.h>

#include <put_batch.h>
#include <constants.h>

using namespace NTpcc;

TEST(PutBatchApi, DefaultLoadBatchRows) {
    EXPECT_EQ(DEFAULT_LOAD_BATCH_ROWS, 2000);
    EXPECT_EQ(EffectiveLoadBatchRows(0), DEFAULT_LOAD_BATCH_ROWS);
    EXPECT_EQ(EffectiveLoadBatchRows(-1), DEFAULT_LOAD_BATCH_ROWS);
    EXPECT_EQ(EffectiveLoadBatchRows(1), 1);
    EXPECT_EQ(EffectiveLoadBatchRows(2000), 2000);
    EXPECT_EQ(EffectiveLoadBatchRows(10000), 10000);
}

TEST(PutBatchApi, KeyRangeAndOutcomes) {
    TLoadKeyRange range;
    range.Table = TABLE_WAREHOUSE;
    range.Begin = 1;
    range.End = 11;
    EXPECT_EQ(range.End - range.Begin, 10);

    TPutBatchResult ok;
    ok.Outcome = EPutBatchOutcome::Completed;
    EXPECT_EQ(ok.Outcome, EPutBatchOutcome::Completed);

    TPutBatchResult fail;
    fail.Outcome = EPutBatchOutcome::Failed;
    fail.Message = "boom";
    EXPECT_EQ(fail.Outcome, EPutBatchOutcome::Failed);
    EXPECT_EQ(fail.Message, "boom");
}

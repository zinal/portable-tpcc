#include <gtest/gtest.h>

#include <histogram.h>

using namespace NTpcc;

TEST(Histogram, TracksMinMaxSumAndAverageInputs) {
    THistogram h(4, 64);
    EXPECT_EQ(h.TotalCount(), 0u);
    EXPECT_EQ(h.MinRecordedValue(), 0u);
    EXPECT_EQ(h.MaxRecordedValue(), 0u);
    EXPECT_EQ(h.SumValues(), 0u);

    h.RecordValue(3);
    h.RecordValue(1);
    h.RecordValue(2);
    h.RecordValue(10);

    EXPECT_EQ(h.TotalCount(), 4u);
    EXPECT_EQ(h.MinRecordedValue(), 1u);
    EXPECT_EQ(h.MaxRecordedValue(), 10u);
    EXPECT_EQ(h.SumValues(), 16u);
}

TEST(Histogram, AddMergesMinMaxSum) {
    THistogram a(4, 64);
    a.RecordValue(5);
    a.RecordValue(2);

    THistogram b(4, 64);
    b.RecordValue(9);
    b.RecordValue(4);

    a.Add(b);
    EXPECT_EQ(a.TotalCount(), 4u);
    EXPECT_EQ(a.MinRecordedValue(), 2u);
    EXPECT_EQ(a.MaxRecordedValue(), 9u);
    EXPECT_EQ(a.SumValues(), 20u);
}

TEST(Histogram, AddEmptyDoesNotClobberMin) {
    THistogram a(4, 64);
    a.RecordValue(7);

    THistogram empty(4, 64);
    a.Add(empty);
    EXPECT_EQ(a.MinRecordedValue(), 7u);
    EXPECT_EQ(a.MaxRecordedValue(), 7u);
    EXPECT_EQ(a.SumValues(), 7u);
}

TEST(Histogram, ResetClearsExtremaAndSum) {
    THistogram h(4, 64);
    h.RecordValue(3);
    h.Reset();
    EXPECT_EQ(h.TotalCount(), 0u);
    EXPECT_EQ(h.MinRecordedValue(), 0u);
    EXPECT_EQ(h.MaxRecordedValue(), 0u);
    EXPECT_EQ(h.SumValues(), 0u);

    h.RecordValue(8);
    EXPECT_EQ(h.MinRecordedValue(), 8u);
    EXPECT_EQ(h.MaxRecordedValue(), 8u);
    EXPECT_EQ(h.SumValues(), 8u);
}

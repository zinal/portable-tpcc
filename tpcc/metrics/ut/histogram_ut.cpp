#include <gtest/gtest.h>

#include <histogram.h>

#include <cmath>

using namespace NTpcc;

TEST(Histogram, TracksMinMaxSumAndAverageInputs) {
    THistogram h(4, 64);
    EXPECT_EQ(h.TotalCount(), 0u);
    EXPECT_EQ(h.MinRecordedValue(), 0u);
    EXPECT_EQ(h.MaxRecordedValue(), 0u);
    EXPECT_EQ(h.SumValues(), 0u);
    EXPECT_EQ(h.OverflowCount(), 0u);

    h.RecordValue(3);
    h.RecordValue(1);
    h.RecordValue(2);
    h.RecordValue(10);

    EXPECT_EQ(h.TotalCount(), 4u);
    EXPECT_EQ(h.MinRecordedValue(), 1u);
    EXPECT_EQ(h.MaxRecordedValue(), 10u);
    EXPECT_EQ(h.SumValues(), 16u);
    EXPECT_EQ(h.OverflowCount(), 0u);
    EXPECT_EQ(h.Buckets().size(), 64u);
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
    EXPECT_EQ(h.OverflowCount(), 0u);
    EXPECT_EQ(h.MinRecordedValue(), 0u);
    EXPECT_EQ(h.MaxRecordedValue(), 0u);
    EXPECT_EQ(h.SumValues(), 0u);

    h.RecordValue(8);
    EXPECT_EQ(h.MinRecordedValue(), 8u);
    EXPECT_EQ(h.MaxRecordedValue(), 8u);
    EXPECT_EQ(h.SumValues(), 8u);
}

TEST(Histogram, OverflowIsSeparateAndDoesNotReplacePercentileWithMax) {
    THistogram h(4, 64);
    for (int i = 0; i < 90; ++i) {
        h.RecordValue(1);
    }
    for (int i = 0; i < 10; ++i) {
        h.RecordValue(100000);
    }

    EXPECT_EQ(h.TotalCount(), 100u);
    EXPECT_EQ(h.OverflowCount(), 10u);
    EXPECT_EQ(h.MaxRecordedValue(), 100000u);

    uint64_t bucketSum = 0;
    for (uint64_t c : h.Buckets()) {
        bucketSum += c;
    }
    EXPECT_EQ(bucketSum + h.OverflowCount(), h.TotalCount());

    EXPECT_EQ(h.GetValueAtPercentile(99), 64u);
    EXPECT_NE(h.GetValueAtPercentile(99), h.MaxRecordedValue());
    EXPECT_EQ(h.GetValueAtPercentile(50), 2u);
}

TEST(Histogram, RelativeErrorStaysWithinTwoPercentPastLinearRegion) {
    THistogram h(4096, 120000000);
    const uint64_t sample = 283000;
    h.RecordValue(sample);

    const uint64_t p99 = h.GetValueAtPercentile(99);
    ASSERT_GE(p99, sample);
    const double rel = static_cast<double>(p99 - sample) / static_cast<double>(sample);
    EXPECT_LE(rel, 0.02) << "p99=" << p99 << " sample=" << sample;
    EXPECT_EQ(h.OverflowCount(), 0u);
    EXPECT_EQ(h.Buckets().size(), 5056u);
}

TEST(Histogram, LinearRegionKeepsUnitResolution) {
    THistogram h(4, 64);
    h.RecordValue(0);
    h.RecordValue(3);
    EXPECT_EQ(h.GetValueAtPercentile(50), 1u);
    EXPECT_EQ(h.GetValueAtPercentile(100), 4u);
}

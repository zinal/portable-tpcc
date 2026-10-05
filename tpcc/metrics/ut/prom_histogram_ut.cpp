#include <prom_histogram.h>

#include <gtest/gtest.h>

namespace NTpcc {

TEST(PromHistogram, BucketAndOverflow) {
    TPromHistogram h;
    h.RecordMicros(500);
    h.RecordMicros(501);
    h.RecordMicros(60000001);
    EXPECT_EQ(h.Count(), 3u);
    EXPECT_EQ(h.Bucket(0), 1u);
    EXPECT_EQ(h.Bucket(1), 1u);
    EXPECT_EQ(h.Overflow(), 1u);
    EXPECT_EQ(h.SumMicros(), 500u + 501u + 60000001u);
}

TEST(PromHistogram, AddAndReset) {
    TPromHistogram a;
    a.RecordMicros(1000);
    TPromHistogram b;
    b.RecordMicros(1000);
    a.Add(b);
    EXPECT_EQ(a.Count(), 2u);
    EXPECT_EQ(a.Bucket(1), 2u);
    a.Reset();
    EXPECT_EQ(a.Count(), 0u);
    EXPECT_EQ(a.SumMicros(), 0u);
    EXPECT_EQ(a.Bucket(1), 0u);
}

TEST(PromHistogram, NegativeClamped) {
    TPromHistogram h;
    h.RecordMicros(-5);
    EXPECT_EQ(h.Count(), 1u);
    EXPECT_EQ(h.Bucket(0), 1u);
    EXPECT_EQ(h.SumMicros(), 0u);
}

} // namespace NTpcc

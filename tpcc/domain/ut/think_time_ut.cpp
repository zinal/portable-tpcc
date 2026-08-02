#include <gtest/gtest.h>

#include <rng.h>
#include <think_time.h>

#include <cmath>
#include <cstdint>

using namespace NTpcc;

TEST(ThinkTime, BenchbaseReturnsConfiguredMean) {
    TSeededRng rng(1);
    EXPECT_EQ(SampleThinkTimeMs(rng.Impl(), 12000, EThinkTimeDistribution::Benchbase), 12000);
    EXPECT_EQ(SampleThinkTimeMs(rng.Impl(), 0, EThinkTimeDistribution::Benchbase), 0);
    EXPECT_EQ(SampleThinkTimeMs(rng.Impl(), -1, EThinkTimeDistribution::Benchbase), 0);
}

TEST(ThinkTime, ExponentialRespectsMeanAndTruncation) {
    TSeededRng rng(42);
    const int64_t meanMs = 12000;
    const int64_t maxMs = 10 * meanMs;

    int64_t minSeen = meanMs;
    int64_t maxSeen = 0;
    double sum = 0.0;
    constexpr int N = 20000;
    for (int i = 0; i < N; ++i) {
        const int64_t tt = SampleThinkTimeMs(rng.Impl(), meanMs, EThinkTimeDistribution::Exponential);
        EXPECT_GE(tt, 0);
        EXPECT_LE(tt, maxMs);
        if (tt < minSeen) {
            minSeen = tt;
        }
        if (tt > maxSeen) {
            maxSeen = tt;
        }
        sum += static_cast<double>(tt);
    }

    const double avg = sum / N;
    // Truncated exponential mean is slightly below the untruncated mean.
    EXPECT_NEAR(avg, static_cast<double>(meanMs), 0.15 * meanMs);
    EXPECT_LT(minSeen, meanMs / 5);
    EXPECT_GT(maxSeen, 3 * meanMs);
}

TEST(ThinkTime, ParseAndStringRoundTrip) {
    EThinkTimeDistribution dist = EThinkTimeDistribution::Benchbase;
    EXPECT_TRUE(ParseThinkTimeDistribution("", dist));
    EXPECT_EQ(dist, EThinkTimeDistribution::Exponential);
    EXPECT_STREQ(ThinkTimeDistributionToString(dist), "exponential");

    EXPECT_TRUE(ParseThinkTimeDistribution("exponential", dist));
    EXPECT_EQ(dist, EThinkTimeDistribution::Exponential);

    EXPECT_TRUE(ParseThinkTimeDistribution("benchbase", dist));
    EXPECT_EQ(dist, EThinkTimeDistribution::Benchbase);
    EXPECT_STREQ(ThinkTimeDistributionToString(dist), "benchbase");

    EXPECT_TRUE(ParseThinkTimeDistribution("constant", dist));
    EXPECT_EQ(dist, EThinkTimeDistribution::Benchbase);

    EXPECT_FALSE(ParseThinkTimeDistribution("uniform", dist));
}

TEST(ThinkTime, RandomUnitIntervalInOpenClosedUnitInterval) {
    TSeededRng rng(7);
    for (int i = 0; i < 1000; ++i) {
        const double r = RandomUnitInterval(rng.Impl());
        EXPECT_GT(r, 0.0);
        EXPECT_LE(r, 1.0);
    }
}

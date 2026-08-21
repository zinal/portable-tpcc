#include <gtest/gtest.h>

#include <run_loop.h>

using namespace NTpcc;

namespace {

bool Observe(TInflightStuckState& state, size_t inflight, size_t ready) {
    return ObserveSchedulerInflightStuck(state, inflight, ready, /*threadCount=*/2, /*maxInflight=*/100);
}

} // namespace

TEST(InflightStuck, HealthyInflightAboveThreadsDoesNotWarn) {
    TInflightStuckState state;
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(Observe(state, /*inflight=*/40, /*ready=*/8));
    }
    EXPECT_EQ(state.ConsecutiveGlued, 0u);
    EXPECT_FALSE(state.Warned);
}

TEST(InflightStuck, NoReadyBacklogDoesNotWarn) {
    TInflightStuckState state;
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(Observe(state, /*inflight=*/2, /*ready=*/2));
    }
    EXPECT_EQ(state.ConsecutiveGlued, 0u);
}

TEST(InflightStuck, NoMaxInflightHeadroomDoesNotWarn) {
    TInflightStuckState state;
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(ObserveSchedulerInflightStuck(
            state, /*inflight=*/2, /*ready=*/64, /*threadCount=*/2, /*maxInflight=*/2));
    }
    EXPECT_EQ(state.ConsecutiveGlued, 0u);
}

TEST(InflightStuck, ZeroInflightDoesNotCount) {
    TInflightStuckState state;
    EXPECT_FALSE(Observe(state, /*inflight=*/0, /*ready=*/64));
    EXPECT_EQ(state.ConsecutiveGlued, 0u);
}

TEST(InflightStuck, InflightAboveThreadsIsNotGlued) {
    TInflightStuckState state;
    EXPECT_FALSE(Observe(state, /*inflight=*/3, /*ready=*/64));
    EXPECT_EQ(state.ConsecutiveGlued, 0u);
}

TEST(InflightStuck, WarnsOnceAfterSustainedWindow) {
    TInflightStuckState state;
    EXPECT_FALSE(Observe(state, 2, 64));
    EXPECT_FALSE(Observe(state, 2, 64));
    EXPECT_EQ(state.ConsecutiveGlued, 2u);
    EXPECT_TRUE(Observe(state, 2, 64));
    EXPECT_TRUE(state.Warned);
    EXPECT_FALSE(Observe(state, 2, 64));
}

TEST(InflightStuck, HealthySampleResetsStreak) {
    TInflightStuckState state;
    EXPECT_FALSE(Observe(state, 2, 64));
    EXPECT_FALSE(Observe(state, 2, 64));
    EXPECT_FALSE(Observe(state, 40, 8));
    EXPECT_EQ(state.ConsecutiveGlued, 0u);
    EXPECT_FALSE(Observe(state, 2, 64));
    EXPECT_FALSE(Observe(state, 2, 64));
    EXPECT_TRUE(Observe(state, 2, 64));
}

TEST(InflightStuck, OceanbaseConfirmedShapeDoesNotWarn) {
    // Observed after PR B: 2 threads, Inflight 30–100, max_inflight=100.
    TInflightStuckState state;
    for (size_t inflight : {30u, 55u, 100u, 42u, 87u}) {
        EXPECT_FALSE(Observe(state, inflight, /*ready=*/20));
    }
    EXPECT_FALSE(state.Warned);
}

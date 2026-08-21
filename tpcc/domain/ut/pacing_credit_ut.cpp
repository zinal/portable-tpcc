#include <gtest/gtest.h>

#include <pacing_credit.h>

#include <chrono>

using namespace NTpcc;
using namespace std::chrono;

TEST(PacingCredit, ApplyReducesRequestedAndCredit) {
    milliseconds credit{5000};
    EXPECT_EQ(ApplyPacingCredit(credit, milliseconds{12000}), milliseconds{7000});
    EXPECT_EQ(credit, milliseconds{0});
}

TEST(PacingCredit, ApplyConsumesPartialCredit) {
    milliseconds credit{3000};
    EXPECT_EQ(ApplyPacingCredit(credit, milliseconds{1000}), milliseconds{0});
    EXPECT_EQ(credit, milliseconds{2000});
}

TEST(PacingCredit, ApplyZeroRequested) {
    milliseconds credit{1000};
    EXPECT_EQ(ApplyPacingCredit(credit, milliseconds{0}), milliseconds{0});
    EXPECT_EQ(credit, milliseconds{1000});
}

TEST(PacingCredit, AccrueOvershootAndCap) {
    using Clock = steady_clock;
    milliseconds credit{0};
    const auto start = Clock::time_point{};
    AccruePacingOvershoot<Clock>(
        credit, milliseconds{10000}, start, milliseconds{1000}, start + milliseconds{4500});
    EXPECT_EQ(credit, milliseconds{3500});

    AccruePacingOvershoot<Clock>(
        credit, milliseconds{10000}, start, milliseconds{1000}, start + milliseconds{20000});
    EXPECT_EQ(credit, milliseconds{10000});
}

TEST(PacingCredit, AccrueNoOvershoot) {
    using Clock = steady_clock;
    milliseconds credit{100};
    const auto start = Clock::time_point{};
    AccruePacingOvershoot<Clock>(
        credit, DefaultPacingCreditCap, start, milliseconds{5000}, start + milliseconds{4000});
    EXPECT_EQ(credit, milliseconds{100});
}

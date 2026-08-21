#include <gtest/gtest.h>

#include <orchestrated_roles.h>

#include <cstddef>
#include <stdexcept>

using namespace NTpcc;

TEST(OrchestratedThreadOverride, MissingKeepsAssignment) {
    size_t threads = 2;
    ApplyOrchestratedThreadOverride(threads, std::nullopt);
    EXPECT_EQ(threads, 2u);
}

TEST(OrchestratedThreadOverride, ZeroMeansAuto) {
    size_t threads = 2;
    ApplyOrchestratedThreadOverride(threads, 0);
    EXPECT_EQ(threads, 0u);
}

TEST(OrchestratedThreadOverride, PositiveReplacesAssignment) {
    size_t threads = 2;
    ApplyOrchestratedThreadOverride(threads, 64);
    EXPECT_EQ(threads, 64u);
}

TEST(OrchestratedThreadOverride, NegativeRejected) {
    size_t threads = 2;
    EXPECT_THROW(ApplyOrchestratedThreadOverride(threads, -1), std::runtime_error);
    EXPECT_EQ(threads, 2u);
}

TEST(OrchestratedThreadOverride, RequireNonNegativeAcceptsMissingAndZero) {
    EXPECT_NO_THROW(RequireNonNegativeThreads(std::nullopt));
    EXPECT_NO_THROW(RequireNonNegativeThreads(0));
    EXPECT_NO_THROW(RequireNonNegativeThreads(64));
    EXPECT_THROW(RequireNonNegativeThreads(-1), std::runtime_error);
}

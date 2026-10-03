#include <gtest/gtest.h>

#include <run_loop.h>

using namespace NTpcc;

TEST(TerminalStartStagger, StandaloneIsOneMillisecond) {
    EXPECT_EQ(kStandaloneTerminalStartStagger.count(), 1);
    EXPECT_EQ(TerminalStartStagger(false).count(), 1);
}

TEST(TerminalStartStagger, StartAtDoesNotStagger) {
    EXPECT_EQ(TerminalStartStagger(true).count(), 0);
}

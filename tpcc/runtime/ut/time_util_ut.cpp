#include <gtest/gtest.h>

#include <time_util.h>

using namespace NTpcc;
using namespace std::chrono;

TEST(TimeUtil, ParseAndFormatRoundTrip) {
    const auto tp = ParseRfc3339Utc("2026-07-28T12:00:15Z");
    EXPECT_EQ(FormatRfc3339Utc(tp), "2026-07-28T12:00:15Z");
}

TEST(TimeUtil, RejectsNonUtc) {
    EXPECT_THROW(ParseRfc3339Utc("2026-07-28T12:00:15"), std::invalid_argument);
    EXPECT_THROW(ParseRfc3339Utc("short"), std::invalid_argument);
}

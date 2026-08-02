#include <gtest/gtest.h>

#include <time_util.h>

using namespace NTpcc;
using namespace std::chrono;

TEST(TimeUtil, ParseAndFormatRoundTrip) {
    const auto tp = ParseRfc3339Utc("2026-07-28T12:00:15Z");
    EXPECT_EQ(FormatRfc3339Utc(tp), "2026-07-28T12:00:15Z");
}

TEST(TimeUtil, ParsesFractionalSeconds) {
    const auto tp = ParseRfc3339Utc("2026-07-28T12:00:15.123456Z");
    EXPECT_EQ(FormatRfc3339Utc(tp), "2026-07-28T12:00:15Z");
}

TEST(TimeUtil, RejectsNonUtc) {
    EXPECT_THROW(ParseRfc3339Utc("2026-07-28T12:00:15"), std::invalid_argument);
    EXPECT_THROW(ParseRfc3339Utc("2026-07-28T12:00:15+03:00"), std::invalid_argument);
    EXPECT_THROW(ParseRfc3339Utc("2026-07-28T12:00:15+03:00Z"), std::invalid_argument);
    EXPECT_THROW(ParseRfc3339Utc("short"), std::invalid_argument);
}

TEST(TimeUtil, RejectsMalformedUtc) {
    EXPECT_THROW(ParseRfc3339Utc("2026-07-28T12:00:15Zjunk"), std::invalid_argument);
    EXPECT_THROW(ParseRfc3339Utc("2026-07-28T12:00:15.Z"), std::invalid_argument);
    EXPECT_THROW(ParseRfc3339Utc("2026-07-28 12:00:15Z"), std::invalid_argument);
}

TEST(TimeUtil, RejectsInvalidCalendarDates) {
    EXPECT_THROW(ParseRfc3339Utc("2026-02-31T12:00:15Z"), std::invalid_argument);
    EXPECT_THROW(ParseRfc3339Utc("2026-13-01T12:00:15Z"), std::invalid_argument);
    EXPECT_THROW(ParseRfc3339Utc("2026-07-28T24:00:00Z"), std::invalid_argument);
}

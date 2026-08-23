#include <clock_calibration.h>

#include <gtest/gtest.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <stdexcept>

using namespace NTpcc;

namespace {

constexpr int64_t kEpochMs = 1'700'000'000'123;

NYdb::TValue PrimitiveTimestamp(int64_t epochMs) {
    return NYdb::TValueBuilder().Timestamp(TInstant::MilliSeconds(epochMs)).Build();
}

NYdb::TValue OptionalTimestamp(int64_t epochMs) {
    return NYdb::TValueBuilder()
        .OptionalTimestamp(TInstant::MilliSeconds(epochMs))
        .Build();
}

} // anonymous

TEST(YdbClockCalibration, ReadsPrimitiveTimestamp) {
    NYdb::TValue value = PrimitiveTimestamp(kEpochMs);
    NYdb::TValueParser parser(value);
    EXPECT_EQ(ServerEpochMsFromTimestampValue(parser), kEpochMs);
}

TEST(YdbClockCalibration, ReadsOptionalTimestamp) {
    NYdb::TValue value = OptionalTimestamp(kEpochMs);
    NYdb::TValueParser parser(value);
    EXPECT_EQ(ServerEpochMsFromTimestampValue(parser), kEpochMs);
}

TEST(YdbClockCalibration, RejectsNullOptionalTimestamp) {
    NYdb::TValue value = NYdb::TValueBuilder()
        .EmptyOptional(NYdb::EPrimitiveType::Timestamp)
        .Build();
    NYdb::TValueParser parser(value);
    EXPECT_THROW(ServerEpochMsFromTimestampValue(parser), std::runtime_error);
}

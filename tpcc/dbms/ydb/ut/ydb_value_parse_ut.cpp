#include <ydb_value_parse.h>

#include <gtest/gtest.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <stdexcept>

using namespace NTpcc;

namespace {

NYdb::TDecimalValue MoneyDecimal(const std::string& text) {
    return NYdb::TDecimalValue(text, 22, 9);
}

NYdb::TValue OptionalDecimal(const std::string& text) {
    return NYdb::TValueBuilder()
        .BeginOptional()
        .Decimal(MoneyDecimal(text))
        .EndOptional()
        .Build();
}

} // anonymous

TEST(YdbValueParse, ReadsPrimitiveInt32) {
    NYdb::TValue value = NYdb::TValueBuilder().Int32(42).Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(Int32FromValue(parser), 42);
}

TEST(YdbValueParse, ReadsOptionalInt32) {
    NYdb::TValue value = NYdb::TValueBuilder().OptionalInt32(7).Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(Int32FromValue(parser), 7);
}

TEST(YdbValueParse, RejectsNullOptionalInt32) {
    NYdb::TValue value = NYdb::TValueBuilder()
        .EmptyOptional(NYdb::EPrimitiveType::Int32)
        .Build();
    NYdb::TValueParser parser(value);
    EXPECT_THROW(Int32FromValue(parser), std::runtime_error);
}

TEST(YdbValueParse, ReadsPrimitiveUtf8) {
    NYdb::TValue value = NYdb::TValueBuilder().Utf8("BAR").Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(Utf8FromValue(parser), "BAR");
}

TEST(YdbValueParse, ReadsOptionalUtf8) {
    NYdb::TValue value = NYdb::TValueBuilder().OptionalUtf8("BAR").Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(Utf8FromValue(parser), "BAR");
}

TEST(YdbValueParse, RejectsNullOptionalUtf8) {
    NYdb::TValue value = NYdb::TValueBuilder()
        .EmptyOptional(NYdb::EPrimitiveType::Utf8)
        .Build();
    NYdb::TValueParser parser(value);
    EXPECT_THROW(Utf8FromValue(parser), std::runtime_error);
}

TEST(YdbValueParse, ReadsPrimitiveDecimal) {
    NYdb::TValue value = NYdb::TValueBuilder().Decimal(MoneyDecimal("12.34")).Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(DecimalFromValue(parser).ToString(), MoneyDecimal("12.34").ToString());
}

TEST(YdbValueParse, ReadsOptionalDecimal) {
    NYdb::TValue value = OptionalDecimal("0.0825");
    NYdb::TValueParser parser(value);
    EXPECT_EQ(DecimalFromValue(parser).ToString(), MoneyDecimal("0.0825").ToString());
}

TEST(YdbValueParse, RejectsNullOptionalDecimal) {
    NYdb::TValue value = NYdb::TValueBuilder()
        .EmptyOptional(NYdb::TTypeBuilder().Decimal(NYdb::TDecimalType(22, 9)).Build())
        .Build();
    NYdb::TValueParser parser(value);
    EXPECT_THROW(DecimalFromValue(parser), std::runtime_error);
}

TEST(YdbValueParse, ReadsUint64Count) {
    NYdb::TValue value = NYdb::TValueBuilder().Uint64(11).Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(CountFromValue(parser), 11u);
}

TEST(YdbValueParse, ReadsOptionalUint64Count) {
    NYdb::TValue value = NYdb::TValueBuilder().OptionalUint64(3).Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(CountFromValue(parser), 3u);
}

TEST(YdbValueParse, ReadsInt64Count) {
    NYdb::TValue value = NYdb::TValueBuilder().Int64(5).Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(CountFromValue(parser), 5u);
}

TEST(YdbValueParse, OptionalInt32AllowsNull) {
    NYdb::TValue value = NYdb::TValueBuilder()
        .EmptyOptional(NYdb::EPrimitiveType::Int32)
        .Build();
    NYdb::TValueParser parser(value);
    EXPECT_FALSE(OptionalInt32FromValue(parser).has_value());
}

TEST(YdbValueParse, OptionalInt32ReadsPrimitive) {
    NYdb::TValue value = NYdb::TValueBuilder().Int32(9).Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(OptionalInt32FromValue(parser), 9);
}

TEST(YdbValueParse, OptionalTimestampReadsPrimitive) {
    const auto ts = TInstant::MilliSeconds(1'700'000'000'000);
    NYdb::TValue value = NYdb::TValueBuilder().Timestamp(ts).Build();
    NYdb::TValueParser parser(value);
    EXPECT_EQ(OptionalTimestampFromValue(parser), ts);
}

TEST(YdbValueParse, OptionalTimestampAllowsNull) {
    NYdb::TValue value = NYdb::TValueBuilder()
        .EmptyOptional(NYdb::EPrimitiveType::Timestamp)
        .Build();
    NYdb::TValueParser parser(value);
    EXPECT_FALSE(OptionalTimestampFromValue(parser).has_value());
}

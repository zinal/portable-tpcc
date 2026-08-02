#include <gtest/gtest.h>

#include <money.h>

using namespace NTpcc;

TEST(TRate, ParseAndFormat) {
    auto rate = TRate::Parse("0.0825");
    EXPECT_EQ(rate.Units(), 825);
    EXPECT_EQ(rate.ToString(), "0.0825");

    EXPECT_EQ(TRate::Parse("0.1"), TRate::FromPermille(100));
    EXPECT_EQ(TRate::Parse("0.5000"), TRate::FromUnits(5000));
    EXPECT_EQ(TRate::Parse("1.0000"), TRate::FromUnits(10000));
}

TEST(TRate, ParseLeadingZero) {
    EXPECT_EQ(TRate::Parse(".0825").Units(), 825);
}

TEST(TRate, ParseNegative) {
    EXPECT_EQ(TRate::Parse("-0.0100").Units(), -100);
}

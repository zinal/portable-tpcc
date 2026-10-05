#include <gtest/gtest.h>

#include <dummy_delay.h>

#include <stdexcept>
#include <string>

using namespace NTpcc;

TEST(DummyDelay, ParsesEmptyAsZero) {
    const auto cfg = ParseDummyConnection("");
    EXPECT_EQ(cfg.MinUs, 0);
    EXPECT_EQ(cfg.MaxUs, 0);
}

TEST(DummyDelay, ParsesMinAndMax) {
    const auto cfg = ParseDummyConnection("delay_us_min=100 delay_us_max=500");
    EXPECT_EQ(cfg.MinUs, 100);
    EXPECT_EQ(cfg.MaxUs, 500);
}

TEST(DummyDelay, MinAloneCopiesToMax) {
    const auto cfg = ParseDummyConnection("delay_us_min=42");
    EXPECT_EQ(cfg.MinUs, 42);
    EXPECT_EQ(cfg.MaxUs, 42);
}

TEST(DummyDelay, MaxAloneKeepsMinZero) {
    const auto cfg = ParseDummyConnection("delay_us_max=9");
    EXPECT_EQ(cfg.MinUs, 0);
    EXPECT_EQ(cfg.MaxUs, 9);
}

TEST(DummyDelay, RejectsUnknownKey) {
    EXPECT_THROW(ParseDummyConnection("foo=1"), std::runtime_error);
}

TEST(DummyDelay, RejectsMaxLessThanMin) {
    EXPECT_THROW(ParseDummyConnection("delay_us_min=10 delay_us_max=1"), std::runtime_error);
}

TEST(DummyDelay, RejectsNegative) {
    EXPECT_THROW(ParseDummyConnection("delay_us_min=-1"), std::runtime_error);
}

TEST(DummyDelay, SampleConstant) {
    TDummyDelayConfig cfg;
    cfg.MinUs = 7;
    cfg.MaxUs = 7;
    EXPECT_EQ(SampleDummyDelay(cfg).count(), 7);
}

TEST(DummyDelay, SampleInRange) {
    TDummyDelayConfig cfg;
    cfg.MinUs = 10;
    cfg.MaxUs = 20;
    for (int i = 0; i < 50; ++i) {
        const auto d = SampleDummyDelay(cfg).count();
        EXPECT_GE(d, 10);
        EXPECT_LE(d, 20);
    }
}

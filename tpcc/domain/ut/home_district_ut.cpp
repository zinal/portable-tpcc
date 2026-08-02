#include <gtest/gtest.h>

#include <constants.h>

using namespace NTpcc;

TEST(HomeDistrictId, DefaultTenTerminalsAreUniqueOneToOne) {
    for (size_t t = 0; t < TERMINALS_PER_WAREHOUSE; ++t) {
        EXPECT_EQ(HomeDistrictId(t), DISTRICT_LOW_ID + static_cast<int>(t));
    }
}

TEST(HomeDistrictId, FewerTerminalsStayDistinct) {
    EXPECT_EQ(HomeDistrictId(0), 1);
    EXPECT_EQ(HomeDistrictId(3), 4);
}

TEST(HomeDistrictId, ExtraTerminalsWrap) {
    EXPECT_EQ(HomeDistrictId(DISTRICT_COUNT), DISTRICT_LOW_ID);
    EXPECT_EQ(HomeDistrictId(DISTRICT_COUNT + 1), DISTRICT_LOW_ID + 1);
}

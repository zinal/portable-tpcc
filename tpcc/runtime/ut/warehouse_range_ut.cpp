#include <gtest/gtest.h>

#include <warehouse_range.h>

using namespace NTpcc;

TEST(WarehouseRange, FormatEmpty) {
    EXPECT_EQ(FormatWarehouseRanges({}), "[]");
}

TEST(WarehouseRange, FormatSingleAndMulti) {
    EXPECT_EQ(FormatWarehouseRanges({{1, 1801}}), "[1,1801)");
    EXPECT_EQ(
        FormatWarehouseRanges({{1, 1801}, {1801, 3601}, {3601, 5401}}),
        "[1,1801),[1801,3601),[3601,5401)");
}

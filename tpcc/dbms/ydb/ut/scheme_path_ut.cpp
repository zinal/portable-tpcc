#include <gtest/gtest.h>

#include <scheme_path.h>

#include <stdexcept>
#include <vector>

using namespace NTpcc;

TEST(YdbSchemePath, ResolveRelativeAndAbsolute) {
    EXPECT_EQ(ResolveYdbAbsolutePath("/rnd-ydb/db1", "tpcc_20k"),
              "/rnd-ydb/db1/tpcc_20k");
    EXPECT_EQ(ResolveYdbAbsolutePath("/rnd-ydb/db1", "/rnd-ydb/db1/tpcc_20k"),
              "/rnd-ydb/db1/tpcc_20k");
    EXPECT_TRUE(ResolveYdbAbsolutePath("/rnd-ydb/db1", "").empty());
}

TEST(YdbSchemePath, InsideDatabaseBoundary) {
    EXPECT_TRUE(IsYdbPathInsideDatabase("/rnd-ydb/db1", "/rnd-ydb/db1"));
    EXPECT_TRUE(IsYdbPathInsideDatabase("/rnd-ydb/db1", "/rnd-ydb/db1/tpcc_20k"));
    EXPECT_FALSE(IsYdbPathInsideDatabase("/rnd-ydb/db1", "/rnd-ydb"));
    EXPECT_FALSE(IsYdbPathInsideDatabase("/rnd-ydb/db1", "/rnd-ydb/db12/x"));
    EXPECT_FALSE(IsYdbPathInsideDatabase("/rnd-ydb/db1", "/other/db1/x"));
}

TEST(YdbSchemePath, DirectoriesOnlyUnderDatabase) {
    const auto dirs = YdbDirectoriesToCreate(
        "/rnd-ydb/db1", "/rnd-ydb/db1/tpcc_20k");
    ASSERT_EQ(dirs.size(), 1u);
    EXPECT_EQ(dirs[0], "/rnd-ydb/db1/tpcc_20k");

    const auto nested = YdbDirectoriesToCreate(
        "/rnd-ydb/db1", "/rnd-ydb/db1/foo/bar");
    ASSERT_EQ(nested.size(), 2u);
    EXPECT_EQ(nested[0], "/rnd-ydb/db1/foo");
    EXPECT_EQ(nested[1], "/rnd-ydb/db1/foo/bar");

    EXPECT_TRUE(YdbDirectoriesToCreate("/rnd-ydb/db1", "/rnd-ydb/db1").empty());
    EXPECT_THROW(
        YdbDirectoriesToCreate("/rnd-ydb/db1", "/rnd-ydb"),
        std::runtime_error);
}

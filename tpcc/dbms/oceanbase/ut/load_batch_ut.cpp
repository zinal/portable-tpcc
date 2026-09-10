#include <gtest/gtest.h>

#include <load_batch.h>

#include <algorithm>
#include <stdexcept>
#include <string>

using namespace NTpcc;

TEST(ObLoadBatchRows, DefaultsWhenNonPositive) {
    EXPECT_EQ(EffectiveObLoadBatchRows(10, 0), static_cast<size_t>(DEFAULT_LOAD_BATCH_ROWS));
    EXPECT_EQ(EffectiveObLoadBatchRows(10, -1), static_cast<size_t>(DEFAULT_LOAD_BATCH_ROWS));
    EXPECT_EQ(DEFAULT_LOAD_BATCH_ROWS, 2000);
}

TEST(ObLoadBatchRows, HonorsPositiveOverride) {
    EXPECT_EQ(EffectiveObLoadBatchRows(3, 1), 1u);
    EXPECT_EQ(EffectiveObLoadBatchRows(3, 2000), 2000u);
    EXPECT_EQ(EffectiveObLoadBatchRows(3, 10000), 10000u);
}

TEST(ObLoadBatchRows, CapsAtPreparedPlaceholderLimit) {
    // 65535 / 10 = 6553 rows, so 10000 is reduced.
    EXPECT_EQ(EffectiveObLoadBatchRows(10, 10000), 6553u);
    EXPECT_EQ(EffectiveObLoadBatchRows(0, 2000), 1u);
}

TEST(ObMultiRowInsertSql, BuildsStablePlaceholderText) {
    const auto sql = BuildObMultiRowInsertSql("stock", {"s_w_id", "s_i_id"}, 2);
    EXPECT_EQ(
        sql,
        "INSERT INTO `stock` (`s_w_id`,`s_i_id`) VALUES (?,?),(?,?)");
    EXPECT_EQ(sql, BuildObMultiRowInsertSql("stock", {"s_w_id", "s_i_id"}, 2));
}

TEST(ObMultiRowInsertSql, AppendsSuffixAndCountsPlaceholders) {
    const auto sql = BuildObMultiRowInsertSql(
        "item",
        {"i_id", "i_name"},
        3,
        " ON DUPLICATE KEY UPDATE i_name = VALUES(i_name)");
    EXPECT_EQ(
        std::count(sql.begin(), sql.end(), '?'),
        6);
    EXPECT_TRUE(sql.find(" ON DUPLICATE KEY UPDATE i_name = VALUES(i_name)") != std::string::npos);
}

TEST(ObMultiRowInsertSql, RejectsEmptyInput) {
    EXPECT_THROW(BuildObMultiRowInsertSql("stock", {"s_w_id"}, 0), std::invalid_argument);
    EXPECT_THROW(BuildObMultiRowInsertSql("stock", {}, 1), std::invalid_argument);
}

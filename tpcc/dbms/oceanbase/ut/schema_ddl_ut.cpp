#include <gtest/gtest.h>

#include <constants.h>
#include <init.h>

#include <stdexcept>
#include <string>
#include <vector>

using namespace NTpcc;

namespace {

const std::string* FindCreateTable(const std::vector<std::string>& stmts, const char* table) {
    const std::string prefix = std::string("CREATE TABLE ") + table + " (";
    for (const auto& sql : stmts) {
        if (sql.compare(0, prefix.size(), prefix) == 0) {
            return &sql;
        }
    }
    return nullptr;
}

} // namespace

TEST(ObSchemaDdl, ItemIsClusterDuplicateTable) {
    TObSchemaLayout layout;
    layout.UseClusterLayout = true;
    layout.DuplicateItem = true;
    layout.PartitionCount = 10;

    const auto stmts = BuildObCreateStatements(layout, TObSchemaOptions{});
    const auto* item = FindCreateTable(stmts, "item");
    ASSERT_NE(item, nullptr);
    EXPECT_NE(item->find("DUPLICATE_SCOPE = 'cluster'"), std::string::npos);
    EXPECT_EQ(item->find("TABLEGROUP"), std::string::npos);
    EXPECT_EQ(item->find("PARTITION BY"), std::string::npos);

    const auto* warehouse = FindCreateTable(stmts, "warehouse");
    ASSERT_NE(warehouse, nullptr);
    EXPECT_NE(warehouse->find("TABLEGROUP = tpcc_tg"), std::string::npos);
    EXPECT_NE(warehouse->find("PARTITION BY HASH(w_id)"), std::string::npos);
}

TEST(ObSchemaDdl, ItemDuplicateWithoutHashPartitions) {
    TObSchemaLayout layout;
    layout.DuplicateItem = true;

    const auto stmts = BuildObCreateStatements(layout, TObSchemaOptions{});
    const auto* item = FindCreateTable(stmts, "item");
    ASSERT_NE(item, nullptr);
    EXPECT_NE(item->find("DUPLICATE_SCOPE = 'cluster'"), std::string::npos);

    const auto* warehouse = FindCreateTable(stmts, "warehouse");
    ASSERT_NE(warehouse, nullptr);
    EXPECT_EQ(warehouse->find("TABLEGROUP"), std::string::npos);
    EXPECT_EQ(warehouse->find("PARTITION BY"), std::string::npos);
}

TEST(ObSchemaDdl, HistoryHistIdIsBigintAutoIncrement) {
    const TObSchemaLayout layouts[] = {
        TObSchemaLayout{},
        TObSchemaLayout{true, true, 64},
    };
    for (const auto& layout : layouts) {
        const auto stmts = BuildObCreateStatements(layout, TObSchemaOptions{});
        const auto* history = FindCreateTable(stmts, "history");
        ASSERT_NE(history, nullptr);
        EXPECT_NE(history->find("hist_id BIGINT"), std::string::npos) << *history;
        EXPECT_NE(history->find("AUTO_INCREMENT"), std::string::npos) << *history;
        EXPECT_EQ(history->find("hist_id INT"), std::string::npos) << *history;
        EXPECT_NE(history->find("PRIMARY KEY (h_w_id, hist_id)"), std::string::npos) << *history;
    }
}

TEST(ObGatherStats, HashPartitionsAreSeparateCallsWithoutHistograms) {
    const auto calls = BuildObGatherTableStatsCalls("tpcc", TABLE_STOCK, 36, 36);
    ASSERT_EQ(calls.size(), 36u);
    EXPECT_NE(calls.front().Sql.find("CALL DBMS_STATS.GATHER_TABLE_STATS('tpcc', 'stock', 'p0'"), std::string::npos);
    EXPECT_EQ(calls.front().Sql.find("estimate_percent"), std::string::npos);
    EXPECT_EQ(calls.front().Sql.find("block_sample"), std::string::npos);
    EXPECT_NE(calls.front().Sql.find("degree=>1"), std::string::npos);
    EXPECT_EQ(calls.front().Sql.find("degree=>36"), std::string::npos);
    EXPECT_NE(calls.front().Sql.find("granularity=>'PARTITION'"), std::string::npos);
    EXPECT_NE(calls.front().Sql.find("method_opt=>'FOR ALL COLUMNS SIZE 1'"), std::string::npos);
    EXPECT_NE(calls.back().Sql.find("'p35'"), std::string::npos);
    EXPECT_NE(calls.front().Label.find("partition p0 (1/36)"), std::string::npos);
    for (const auto& call : calls) {
        EXPECT_EQ(call.Sql.find("SIZE AUTO"), std::string::npos) << call.Sql;
    }
}

TEST(ObGatherStats, ItemAndPlainTablesStayOneCall) {
    const auto item = BuildObGatherTableStatsCalls("tpcc", TABLE_ITEM, 36, 36);
    ASSERT_EQ(item.size(), 1u);
    EXPECT_NE(item[0].Sql.find("CALL DBMS_STATS.GATHER_TABLE_STATS('tpcc', 'item', degree=>36"), std::string::npos);
    EXPECT_EQ(item[0].Sql.find("estimate_percent"), std::string::npos);
    EXPECT_EQ(item[0].Sql.find("granularity"), std::string::npos);
    EXPECT_NE(item[0].Sql.find("method_opt=>'FOR ALL COLUMNS SIZE 1'"), std::string::npos);

    const auto plain = BuildObGatherTableStatsCalls("tpcc", TABLE_CUSTOMER, 1, -1);
    ASSERT_EQ(plain.size(), 1u);
    EXPECT_NE(plain[0].Sql.find("degree=>1"), std::string::npos);
    EXPECT_EQ(plain[0].Sql.find("'p0'"), std::string::npos);
}

TEST(ObGatherStats, ParallelSessionsFollowPartitionCount) {
    EXPECT_EQ(ObStatsGatherSessionCount(-1), 1);
    EXPECT_EQ(ObStatsGatherSessionCount(1), 1);
    EXPECT_EQ(ObStatsGatherSessionCount(36), 36);
    EXPECT_EQ(ObStatsGatherSessionCount(OB_MAX_PARALLEL_STATS_GATHERS), OB_MAX_PARALLEL_STATS_GATHERS);
    EXPECT_EQ(ObStatsGatherSessionCount(OB_MAX_PARALLEL_STATS_GATHERS + 100), OB_MAX_PARALLEL_STATS_GATHERS);
}

TEST(ObGatherStats, RejectsEmptyTableOrDegree) {
    EXPECT_THROW(BuildObGatherTableStatsCalls("tpcc", "", 1, -1), std::runtime_error);
    EXPECT_THROW(BuildObGatherTableStatsCalls("tpcc", TABLE_STOCK, 0, 4), std::runtime_error);
    EXPECT_THROW(BuildObPartitionColumnStatsQuery("tpcc", ""), std::runtime_error);
    EXPECT_THROW(BuildObPartitionColumnStatsQuery("tpcc", nullptr), std::runtime_error);
}

TEST(ObGatherStats, ColumnStatsComeFromPartitionView) {
    const auto sql = BuildObPartitionColumnStatsQuery("tpcc", TABLE_STOCK);
    EXPECT_NE(sql.find("FROM oceanbase.DBA_PART_COL_STATISTICS"), std::string::npos) << sql;
    EXPECT_EQ(sql.find("DBA_TAB_COL_STATISTICS"), std::string::npos) << sql;
    EXPECT_NE(sql.find("WHERE owner = 'tpcc' AND table_name = 'stock'"), std::string::npos) << sql;
    EXPECT_NE(sql.find("partition_name IS NOT NULL"), std::string::npos) << sql;
    EXPECT_NE(sql.find("GROUP BY column_name"), std::string::npos) << sql;
    EXPECT_NE(sql.find("SUM(num_distinct)"), std::string::npos) << sql;
    EXPECT_NE(sql.find("MAX(num_distinct)"), std::string::npos) << sql;
}

TEST(ObSchemaDdl, ItemPlainWhenDuplicateDisabled) {
    TObSchemaLayout layout;

    const auto stmts = BuildObCreateStatements(layout, TObSchemaOptions{});
    const auto* item = FindCreateTable(stmts, "item");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->find("DUPLICATE_SCOPE"), std::string::npos);
    EXPECT_EQ(item->find("TABLEGROUP"), std::string::npos);
}

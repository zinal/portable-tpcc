#include <gtest/gtest.h>

#include <init.h>

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

TEST(ObSchemaDdl, ItemPlainWhenDuplicateDisabled) {
    TObSchemaLayout layout;

    const auto stmts = BuildObCreateStatements(layout, TObSchemaOptions{});
    const auto* item = FindCreateTable(stmts, "item");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->find("DUPLICATE_SCOPE"), std::string::npos);
    EXPECT_EQ(item->find("TABLEGROUP"), std::string::npos);
}

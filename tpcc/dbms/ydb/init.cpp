#include "init.h"

#include "data_splitter.h"
#include "scheme_path.h"

#include <constants.h>
#include <log.h>

#include <fmt/format.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/scheme/scheme.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace NTpcc {

namespace {

std::string PartitionClause(const std::vector<int>& splitKeys, int minParts, int maxParts) {
    std::ostringstream clause;
    clause << "WITH (\n"
           << "    AUTO_PARTITIONING_BY_LOAD = DISABLED,\n"
           << "    AUTO_PARTITIONING_PARTITION_SIZE_MB = 2048,\n"
           << "    AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = " << minParts << ",\n"
           << "    AUTO_PARTITIONING_MAX_PARTITIONS_COUNT = " << maxParts;
    if (!splitKeys.empty()) {
        clause << ",\n    PARTITION_AT_KEYS = (";
        for (size_t i = 0; i < splitKeys.size(); ++i) {
            if (i) {
                clause << ", ";
            }
            clause << splitKeys[i];
        }
        clause << ")";
    }
    clause << "\n)";
    return clause.str();
}

void ThrowIfFailed(const NYdb::TStatus& status, const std::string& what) {
    if (!status.IsSuccess()) {
        throw std::runtime_error(what + ": " + status.GetIssues().ToOneLineString());
    }
}

void Exec(NYdb::NQuery::TQueryClient& client, const std::string& sql, const std::string& what) {
    LOG_T(sql);
    auto status = client.RetryQuerySync([&](NYdb::NQuery::TSession session) {
        return session.ExecuteQuery(sql, NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
    });
    ThrowIfFailed(status, what);
}

void EnsurePath(NYdb::TDriver& driver, const TYdbConnectionConfig& config) {
    if (config.Path.empty()) {
        return;
    }

    // Absolute --path values must stay under --database. Walking from "/" would
    // attempt MakeDirectory on parents outside the database (e.g. /rnd-ydb when
    // database is /rnd-ydb/db1), which YDB rejects with "Table path not in database".
    const std::string path = ResolveYdbAbsolutePath(config.Database, config.Path);
    NYdb::NScheme::TSchemeClient scheme(driver);
    for (const auto& current : YdbDirectoriesToCreate(config.Database, path)) {
        auto status = scheme.MakeDirectory(current).GetValueSync();
        if (!status.IsSuccess() && status.GetStatus() != NYdb::EStatus::ALREADY_EXISTS) {
            ThrowIfFailed(status, "failed to create YDB directory " + current);
        }
    }
}

} // anonymous

void InitSync(const TYdbConnectionConfig& connectionConfig, int warehouseCount) {
    LOG_I("Initializing YDB TPC-C schema...");

    TYdbConnection connection(connectionConfig);
    EnsurePath(connection.Driver(), connectionConfig);
    auto& client = connection.QueryClient();

    TDataSplitter splitter(warehouseCount);
    const int minParts = TDataSplitter::CalcMinParts(warehouseCount);
    const auto small = PartitionClause(
        splitter.GetSplitKeys(TABLE_WAREHOUSE), minParts,
        std::max(minParts, static_cast<int>(splitter.GetSplitKeys(TABLE_WAREHOUSE).size()) + 1) * 2);
    const auto item = PartitionClause(
        splitter.GetSplitKeys(TABLE_ITEM), minParts,
        std::max(minParts, static_cast<int>(splitter.GetSplitKeys(TABLE_ITEM).size()) + 1) * 2);
    const auto stock = PartitionClause(
        splitter.GetSplitKeys(TABLE_STOCK), minParts,
        std::max(minParts, static_cast<int>(splitter.GetSplitKeys(TABLE_STOCK).size()) + 1) * 2);
    const auto customer = PartitionClause(
        splitter.GetSplitKeys(TABLE_CUSTOMER), minParts,
        std::max(minParts, static_cast<int>(splitter.GetSplitKeys(TABLE_CUSTOMER).size()) + 1) * 2);
    const auto history = PartitionClause(
        splitter.GetSplitKeys(TABLE_HISTORY), minParts,
        std::max(minParts, static_cast<int>(splitter.GetSplitKeys(TABLE_HISTORY).size()) + 1) * 2);
    const auto oorder = PartitionClause(
        splitter.GetSplitKeys(TABLE_OORDER), minParts,
        std::max(minParts, static_cast<int>(splitter.GetSplitKeys(TABLE_OORDER).size()) + 1) * 2);
    const auto orderLine = PartitionClause(
        splitter.GetSplitKeys(TABLE_ORDER_LINE), minParts,
        std::max(minParts, static_cast<int>(splitter.GetSplitKeys(TABLE_ORDER_LINE).size()) + 1) * 2);

    const std::string pragma = fmt::format("PRAGMA TablePathPrefix(\"{}\");\n", connectionConfig.Path);

    Exec(client, pragma + fmt::format(R"(
        CREATE TABLE `warehouse` (
            w_id Int32 NOT NULL,
            w_ytd Decimal(22,9),
            w_tax Decimal(22,9),
            w_name Utf8,
            w_street_1 Utf8,
            w_street_2 Utf8,
            w_city Utf8,
            w_state Utf8,
            w_zip Utf8,
            PRIMARY KEY (w_id)
        ) {};
    )", small), "create warehouse");

    Exec(client, pragma + fmt::format(R"(
        CREATE TABLE `item` (
            i_id Int32 NOT NULL,
            i_name Utf8,
            i_price Decimal(22,9),
            i_data Utf8,
            i_im_id Int32,
            PRIMARY KEY (i_id)
        ) {};
    )", item), "create item");

    Exec(client, pragma + fmt::format(R"(
        CREATE TABLE `stock` (
            s_w_id Int32 NOT NULL,
            s_i_id Int32 NOT NULL,
            s_quantity Int32,
            s_ytd Decimal(22,9),
            s_order_cnt Int32,
            s_remote_cnt Int32,
            s_data Utf8,
            s_dist_01 Utf8,
            s_dist_02 Utf8,
            s_dist_03 Utf8,
            s_dist_04 Utf8,
            s_dist_05 Utf8,
            s_dist_06 Utf8,
            s_dist_07 Utf8,
            s_dist_08 Utf8,
            s_dist_09 Utf8,
            s_dist_10 Utf8,
            PRIMARY KEY (s_w_id, s_i_id)
        ) {};
    )", stock), "create stock");

    Exec(client, pragma + fmt::format(R"(
        CREATE TABLE `district` (
            d_w_id Int32 NOT NULL,
            d_id Int32 NOT NULL,
            d_ytd Decimal(22,9),
            d_tax Decimal(22,9),
            d_next_o_id Int32,
            d_name Utf8,
            d_street_1 Utf8,
            d_street_2 Utf8,
            d_city Utf8,
            d_state Utf8,
            d_zip Utf8,
            PRIMARY KEY (d_w_id, d_id)
        ) {};
    )", small), "create district");

    Exec(client, pragma + fmt::format(R"(
        CREATE TABLE `customer` (
            c_w_id Int32 NOT NULL,
            c_d_id Int32 NOT NULL,
            c_id Int32 NOT NULL,
            c_discount Decimal(22,9),
            c_credit Utf8,
            c_last Utf8,
            c_first Utf8,
            c_credit_lim Decimal(22,9),
            c_balance Decimal(22,9),
            c_ytd_payment Decimal(22,9),
            c_payment_cnt Int32,
            c_delivery_cnt Int32,
            c_street_1 Utf8,
            c_street_2 Utf8,
            c_city Utf8,
            c_state Utf8,
            c_zip Utf8,
            c_phone Utf8,
            c_since Timestamp,
            c_middle Utf8,
            c_data Utf8,
            PRIMARY KEY (c_w_id, c_d_id, c_id)
        ) {};
    )", customer), "create customer");

    Exec(client, pragma + fmt::format(R"(
        CREATE TABLE `history` (
            h_c_w_id Int32 NOT NULL,
            h_c_d_id Int32 NOT NULL,
            h_c_id Int32 NOT NULL,
            h_c_nano_ts Int64 NOT NULL,
            h_d_id Int32,
            h_w_id Int32,
            h_date Timestamp,
            h_amount Decimal(22,9),
            h_data Utf8,
            PRIMARY KEY (h_c_w_id, h_c_d_id, h_c_id, h_c_nano_ts)
        ) {};
    )", history), "create history");

    Exec(client, pragma + fmt::format(R"(
        CREATE TABLE `oorder` (
            o_w_id Int32 NOT NULL,
            o_d_id Int32 NOT NULL,
            o_id Int32 NOT NULL,
            o_c_id Int32,
            o_carrier_id Int32,
            o_ol_cnt Int32,
            o_all_local Int32,
            o_entry_d Timestamp,
            PRIMARY KEY (o_w_id, o_d_id, o_id)
        ) {};
    )", oorder), "create oorder");

    Exec(client, pragma + fmt::format(R"(
        CREATE TABLE `new_order` (
            no_w_id Int32 NOT NULL,
            no_d_id Int32 NOT NULL,
            no_o_id Int32 NOT NULL,
            PRIMARY KEY (no_w_id, no_d_id, no_o_id)
        ) {};
    )", small), "create new_order");

    Exec(client, pragma + fmt::format(R"(
        CREATE TABLE `order_line` (
            ol_w_id Int32 NOT NULL,
            ol_d_id Int32 NOT NULL,
            ol_o_id Int32 NOT NULL,
            ol_number Int32 NOT NULL,
            ol_i_id Int32,
            ol_delivery_d Timestamp,
            ol_amount Decimal(22,9),
            ol_supply_w_id Int32,
            ol_quantity Int32,
            ol_dist_info Utf8,
            PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)
        ) {};
    )", orderLine), "create order_line");

    LOG_I("All YDB TPC-C tables created successfully");
}

void CreateIndexes(const TYdbConnectionConfig& connectionConfig) {
    LOG_I("Creating YDB secondary indexes...");
    TYdbConnection connection(connectionConfig);
    auto& client = connection.QueryClient();
    const std::string pragma = fmt::format("PRAGMA TablePathPrefix(\"{}\");\n", connectionConfig.Path);
    Exec(client, pragma + R"(
        ALTER TABLE `customer` ADD INDEX `idx_customer_name` GLOBAL ON (c_w_id, c_d_id, c_last, c_first);
    )", "create idx_customer_name");
    Exec(client, pragma + R"(
        ALTER TABLE `oorder` ADD INDEX `idx_order` GLOBAL ON (o_w_id, o_d_id, o_c_id, o_id);
    )", "create idx_order");
}

} // namespace NTpcc

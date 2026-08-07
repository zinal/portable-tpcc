#include "init.h"

#include "data_splitter.h"

#include <constants.h>
#include <log.h>

#include <fmt/format.h>

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

} // anonymous

void InitSync(const TYdbConnectionConfig& connectionConfig, int warehouseCount) {
    LOG_I("Initializing YDB TPC-C schema...");

    TYdbConnection connection(connectionConfig);
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

    // CREATE TABLE must use a database-relative path in the table name.
    // TablePathPrefix is not applied correctly for CREATE TABLE.
    const std::string warehousePath = connection.RelativeTablePath(TABLE_WAREHOUSE);
    const std::string itemPath = connection.RelativeTablePath(TABLE_ITEM);
    const std::string stockPath = connection.RelativeTablePath(TABLE_STOCK);
    const std::string districtPath = connection.RelativeTablePath(TABLE_DISTRICT);
    const std::string customerPath = connection.RelativeTablePath(TABLE_CUSTOMER);
    const std::string historyPath = connection.RelativeTablePath(TABLE_HISTORY);
    const std::string oorderPath = connection.RelativeTablePath(TABLE_OORDER);
    const std::string newOrderPath = connection.RelativeTablePath(TABLE_NEW_ORDER);
    const std::string orderLinePath = connection.RelativeTablePath(TABLE_ORDER_LINE);

    Exec(client, fmt::format(R"(
        CREATE TABLE `{}` (
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
    )", warehousePath, small), "create warehouse");

    Exec(client, fmt::format(R"(
        CREATE TABLE `{}` (
            i_id Int32 NOT NULL,
            i_name Utf8,
            i_price Decimal(22,9),
            i_data Utf8,
            i_im_id Int32,
            PRIMARY KEY (i_id)
        ) {};
    )", itemPath, item), "create item");

    Exec(client, fmt::format(R"(
        CREATE TABLE `{}` (
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
    )", stockPath, stock), "create stock");

    Exec(client, fmt::format(R"(
        CREATE TABLE `{}` (
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
    )", districtPath, small), "create district");

    Exec(client, fmt::format(R"(
        CREATE TABLE `{}` (
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
    )", customerPath, customer), "create customer");

    Exec(client, fmt::format(R"(
        CREATE TABLE `{}` (
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
    )", historyPath, history), "create history");

    Exec(client, fmt::format(R"(
        CREATE TABLE `{}` (
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
    )", oorderPath, oorder), "create oorder");

    Exec(client, fmt::format(R"(
        CREATE TABLE `{}` (
            no_w_id Int32 NOT NULL,
            no_d_id Int32 NOT NULL,
            no_o_id Int32 NOT NULL,
            PRIMARY KEY (no_w_id, no_d_id, no_o_id)
        ) {};
    )", newOrderPath, small), "create new_order");

    Exec(client, fmt::format(R"(
        CREATE TABLE `{}` (
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
    )", orderLinePath, orderLine), "create order_line");

    LOG_I("All YDB TPC-C tables created successfully");
}

void CreateIndexes(const TYdbConnectionConfig& connectionConfig) {
    LOG_I("Creating YDB secondary indexes...");
    TYdbConnection connection(connectionConfig);
    auto& client = connection.QueryClient();
    Exec(client, fmt::format(R"(
        ALTER TABLE `{}` ADD INDEX `idx_customer_name` GLOBAL ON (c_w_id, c_d_id, c_last, c_first);
    )", connection.RelativeTablePath(TABLE_CUSTOMER)), "create idx_customer_name");
    Exec(client, fmt::format(R"(
        ALTER TABLE `{}` ADD INDEX `idx_order` GLOBAL ON (o_w_id, o_d_id, o_c_id, o_id);
    )", connection.RelativeTablePath(TABLE_OORDER)), "create idx_order");
}

} // namespace NTpcc

#include "init.h"

#include <constants.h>
#include <log.h>
#include <domain_util.h>

#include <pqxx/pqxx>
#include <fmt/format.h>

#include <sstream>
#include <stdexcept>

namespace NTpcc {

namespace {

std::string HashPartitionClause(const char* column, int partitionCount) {
    if (partitionCount <= 0) {
        return {};
    }
    return fmt::format(" PARTITION BY HASH ({})", column);
}

std::string HashPartitionChildren(const char* table, int partitionCount) {
    if (partitionCount <= 0) {
        return {};
    }
    std::ostringstream out;
    for (int r = 0; r < partitionCount; ++r) {
        out << fmt::format(
            "CREATE TABLE {}_p{} PARTITION OF {} FOR VALUES WITH (MODULUS {}, REMAINDER {});\n",
            table, r, table, partitionCount, r);
    }
    return out.str();
}

} // anonymous

std::string BuildTpccSchemaDdl(int hashPartitionCount, bool enableForeignKeys) {
    const std::string stockPart = HashPartitionClause("s_w_id", hashPartitionCount);
    const std::string customerPart = HashPartitionClause("c_w_id", hashPartitionCount);
    const std::string oorderPart = HashPartitionClause("o_w_id", hashPartitionCount);
    const std::string newOrderPart = HashPartitionClause("no_w_id", hashPartitionCount);
    const std::string orderLinePart = HashPartitionClause("ol_w_id", hashPartitionCount);
    const std::string historyPart = HashPartitionClause("h_w_id", hashPartitionCount);

    const std::string fkStockWarehouse = enableForeignKeys
        ? "    FOREIGN KEY (s_w_id) REFERENCES warehouse (w_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkStockItem = enableForeignKeys
        ? "    FOREIGN KEY (s_i_id) REFERENCES item (i_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkDistrict = enableForeignKeys
        ? "    FOREIGN KEY (d_w_id) REFERENCES warehouse (w_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkCustomer = enableForeignKeys
        ? "    FOREIGN KEY (c_w_id, c_d_id) REFERENCES district (d_w_id, d_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkHistoryCustomer = enableForeignKeys
        ? "    FOREIGN KEY (h_c_w_id, h_c_d_id, h_c_id) REFERENCES customer (c_w_id, c_d_id, c_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkHistoryDistrict = enableForeignKeys
        ? "    FOREIGN KEY (h_w_id, h_d_id) REFERENCES district (d_w_id, d_id) ON DELETE CASCADE"
        : "";
    const std::string historyDataSuffix = enableForeignKeys ? "," : "";
    const std::string fkOorder = enableForeignKeys
        ? "    FOREIGN KEY (o_w_id, o_d_id, o_c_id) REFERENCES customer (c_w_id, c_d_id, c_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkNewOrder = enableForeignKeys
        ? "    FOREIGN KEY (no_w_id, no_d_id, no_o_id) REFERENCES oorder (o_w_id, o_d_id, o_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkOrderLineOorder = enableForeignKeys
        ? "    FOREIGN KEY (ol_w_id, ol_d_id, ol_o_id) REFERENCES oorder (o_w_id, o_d_id, o_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkOrderLineStock = enableForeignKeys
        ? "    FOREIGN KEY (ol_supply_w_id, ol_i_id) REFERENCES stock (s_w_id, s_i_id) ON DELETE CASCADE,\n"
        : "";

    std::ostringstream ddl;
    ddl << R"(
DROP TABLE IF EXISTS history CASCADE;
DROP TABLE IF EXISTS new_order CASCADE;
DROP TABLE IF EXISTS order_line CASCADE;
DROP TABLE IF EXISTS oorder CASCADE;
DROP TABLE IF EXISTS customer CASCADE;
DROP TABLE IF EXISTS district CASCADE;
DROP TABLE IF EXISTS stock CASCADE;
DROP TABLE IF EXISTS item CASCADE;
DROP TABLE IF EXISTS warehouse CASCADE;

CREATE TABLE warehouse (
    w_id       int            NOT NULL,
    w_ytd      decimal(12, 2) NOT NULL,
    w_tax      decimal(4, 4)  NOT NULL,
    w_name     varchar(10)    NOT NULL,
    w_street_1 varchar(20)    NOT NULL,
    w_street_2 varchar(20)    NOT NULL,
    w_city     varchar(20)    NOT NULL,
    w_state    char(2)        NOT NULL,
    w_zip      char(9)        NOT NULL,
    PRIMARY KEY (w_id)
);

CREATE TABLE item (
    i_id    int           NOT NULL,
    i_name  varchar(24)   NOT NULL,
    i_price decimal(5, 2) NOT NULL,
    i_data  varchar(50)   NOT NULL,
    i_im_id int           NOT NULL,
    PRIMARY KEY (i_id)
);

CREATE TABLE stock (
    s_w_id       int           NOT NULL,
    s_i_id       int           NOT NULL,
    s_quantity   int           NOT NULL,
    s_ytd        decimal(8, 0) NOT NULL,
    s_order_cnt  int           NOT NULL,
    s_remote_cnt int           NOT NULL,
    s_data       varchar(50)   NOT NULL,
    s_dist_01    char(24)      NOT NULL,
    s_dist_02    char(24)      NOT NULL,
    s_dist_03    char(24)      NOT NULL,
    s_dist_04    char(24)      NOT NULL,
    s_dist_05    char(24)      NOT NULL,
    s_dist_06    char(24)      NOT NULL,
    s_dist_07    char(24)      NOT NULL,
    s_dist_08    char(24)      NOT NULL,
    s_dist_09    char(24)      NOT NULL,
    s_dist_10    char(24)      NOT NULL,
)" << fkStockWarehouse << fkStockItem << R"(    PRIMARY KEY (s_w_id, s_i_id)
)
)" << stockPart << R"(;

CREATE TABLE district (
    d_w_id      int            NOT NULL,
    d_id        int            NOT NULL,
    d_ytd       decimal(12, 2) NOT NULL,
    d_tax       decimal(4, 4)  NOT NULL,
    d_next_o_id int            NOT NULL,
    d_name      varchar(10)    NOT NULL,
    d_street_1  varchar(20)    NOT NULL,
    d_street_2  varchar(20)    NOT NULL,
    d_city      varchar(20)    NOT NULL,
    d_state     char(2)        NOT NULL,
    d_zip       char(9)        NOT NULL,
)" << fkDistrict << R"(    PRIMARY KEY (d_w_id, d_id)
);

CREATE TABLE customer (
    c_w_id         int            NOT NULL,
    c_d_id         int            NOT NULL,
    c_id           int            NOT NULL,
    c_discount     decimal(4, 4)  NOT NULL,
    c_credit       char(2)        NOT NULL,
    c_last         varchar(16)    NOT NULL,
    c_first        varchar(16)    NOT NULL,
    c_credit_lim   decimal(12, 2) NOT NULL,
    c_balance      decimal(12, 2) NOT NULL,
    c_ytd_payment  decimal(12, 2) NOT NULL,
    c_payment_cnt  int            NOT NULL,
    c_delivery_cnt int            NOT NULL,
    c_street_1     varchar(20)    NOT NULL,
    c_street_2     varchar(20)    NOT NULL,
    c_city         varchar(20)    NOT NULL,
    c_state        char(2)        NOT NULL,
    c_zip          char(9)        NOT NULL,
    c_phone        char(16)       NOT NULL,
    c_since        timestamp      NOT NULL DEFAULT CURRENT_TIMESTAMP,
    c_middle       char(2)        NOT NULL,
    c_data         varchar(500)   NOT NULL,
)" << fkCustomer << R"(    PRIMARY KEY (c_w_id, c_d_id, c_id)
)
)" << customerPart << R"(;

CREATE TABLE history (
    h_c_id   int           NOT NULL,
    h_c_d_id int           NOT NULL,
    h_c_w_id int           NOT NULL,
    h_d_id   int           NOT NULL,
    h_w_id   int           NOT NULL,
    h_date   timestamp     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    h_amount decimal(6, 2) NOT NULL,
    h_data   varchar(24)   NOT NULL)" << historyDataSuffix << R"(
)" << fkHistoryCustomer << fkHistoryDistrict << R"(
)
)" << historyPart << R"(;

CREATE TABLE oorder (
    o_w_id       int       NOT NULL,
    o_d_id       int       NOT NULL,
    o_id         int       NOT NULL,
    o_c_id       int       NOT NULL,
    o_carrier_id int                DEFAULT NULL,
    o_ol_cnt     int       NOT NULL,
    o_all_local  int       NOT NULL,
    o_entry_d    timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (o_w_id, o_d_id, o_id),
)" << fkOorder << R"(    CONSTRAINT idx_order UNIQUE (o_w_id, o_d_id, o_c_id, o_id)
)
)" << oorderPart << R"(;

CREATE TABLE new_order (
    no_w_id int NOT NULL,
    no_d_id int NOT NULL,
    no_o_id int NOT NULL,
)" << fkNewOrder << R"(    PRIMARY KEY (no_w_id, no_d_id, no_o_id)
)
)" << newOrderPart << R"(;

CREATE TABLE order_line (
    ol_w_id        int           NOT NULL,
    ol_d_id        int           NOT NULL,
    ol_o_id        int           NOT NULL,
    ol_number      int           NOT NULL,
    ol_i_id        int           NOT NULL,
    ol_delivery_d  timestamp     NULL DEFAULT NULL,
    ol_amount      decimal(6, 2) NOT NULL,
    ol_supply_w_id int           NOT NULL,
    ol_quantity    decimal(6, 2) NOT NULL,
    ol_dist_info   char(24)      NOT NULL,
)" << fkOrderLineOorder << fkOrderLineStock << R"(    PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)
)
)" << orderLinePart << ";\n";

    ddl << HashPartitionChildren(TABLE_STOCK, hashPartitionCount);
    ddl << HashPartitionChildren(TABLE_CUSTOMER, hashPartitionCount);
    ddl << HashPartitionChildren(TABLE_HISTORY, hashPartitionCount);
    ddl << HashPartitionChildren(TABLE_OORDER, hashPartitionCount);
    ddl << HashPartitionChildren(TABLE_NEW_ORDER, hashPartitionCount);
    ddl << HashPartitionChildren(TABLE_ORDER_LINE, hashPartitionCount);

    return ddl.str();
}

const char* INDEX_DDL = R"(
CREATE INDEX IF NOT EXISTS idx_customer_name ON customer (c_w_id, c_d_id, c_last, c_first);
)";

void SetSearchPath(pqxx::connection& conn, const std::string& path) {
    if (!path.empty()) {
        pqxx::nontransaction ntx(conn);
        ntx.exec(fmt::format("SET search_path TO {}", conn.quote_name(path)));
    }
}

void InitSync(
    const std::string& connectionString,
    const std::string& path,
    const TPgPartitionConfig& partitionConfig)
{
    const int hashPartitions = ResolvePgPartitionCount(partitionConfig);

    if (hashPartitions > 0) {
        LOG_I("Initializing TPC-C schema with warehouse_hash partitioning ("
              << hashPartitions << " partitions)...");
    } else {
        LOG_I("Initializing TPC-C schema...");
    }
    LOG_I("Foreign keys: " << ForeignKeysModeLabel(partitionConfig.EnableForeignKeys));

    try {
        pqxx::connection conn(connectionString);

        if (!path.empty()) {
            pqxx::nontransaction ntx(conn);
            ntx.exec(fmt::format("CREATE SCHEMA IF NOT EXISTS {}", conn.quote_name(path)));
            LOG_I("Created schema '" << path << "'");
        }

        SetSearchPath(conn, path);

        const std::string ddl = BuildTpccSchemaDdl(
            hashPartitions, partitionConfig.EnableForeignKeys);
        pqxx::nontransaction ntx(conn);
        ntx.exec(ddl);
        LOG_I("All TPC-C tables created successfully");
    } catch (const std::exception& e) {
        LOG_E("Failed to create TPC-C tables: " << e.what());
        LOG_E("After fixing the reason, you might need to run `tpcc drop`.");
        throw;
    }
}

void CreateIndexes(const std::string& connectionString, const std::string& path) {
    LOG_I("Creating secondary indexes...");

    try {
        pqxx::connection conn(connectionString);
        SetSearchPath(conn, path);

        pqxx::nontransaction ntx(conn);
        ntx.exec(INDEX_DDL);
        LOG_I("Secondary indexes created");
    } catch (const std::exception& e) {
        LOG_E("Failed to create indexes: " << e.what());
        throw;
    }
}

} // namespace NTpcc

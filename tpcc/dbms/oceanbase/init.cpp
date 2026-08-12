#include "init.h"

#include "ob_connection.h"
#include "ob_errors.h"

#include <constants.h>
#include <log.h>

#include <fmt/format.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace NTpcc {

namespace {

constexpr const char* TABLEGROUP_TPCC = "tpcc_tg";

const char* const DROP_TABLES[] = {
    "DROP TABLE IF EXISTS history",
    "DROP TABLE IF EXISTS new_order",
    "DROP TABLE IF EXISTS order_line",
    "DROP TABLE IF EXISTS oorder",
    "DROP TABLE IF EXISTS customer",
    "DROP TABLE IF EXISTS district",
    "DROP TABLE IF EXISTS stock",
    "DROP TABLE IF EXISTS item",
    "DROP TABLE IF EXISTS warehouse",
};

struct TSchemaLayout {
    bool UseClusterLayout = false;
    int PartitionCount = 1;
};

void ExecAll(TObConnection& conn, const std::vector<std::string>& statements) {
    for (const auto& sql : statements) {
        conn.ExecuteSimple(sql);
    }
}

TSchemaLayout ResolveSchemaLayout(TObConnection& conn, const TObSchemaOptions& options) {
    const int partitions = ResolveObPartitionCount(options);
    TSchemaLayout layout;
    if (partitions < 0) {
        return layout;
    }
    if (!IsOceanBaseServer(conn)) {
        if (options.PartitionCount > 0) {
            LOG_W("Ignoring OceanBase partitions: target is not OceanBase");
        }
        return layout;
    }
    layout.UseClusterLayout = true;
    layout.PartitionCount = partitions;
    return layout;
}

std::string ClusterTableSuffix(const TSchemaLayout& layout, const char* hashColumn) {
    if (!layout.UseClusterLayout) {
        return {};
    }
    return fmt::format(
        " TABLEGROUP = {} PARTITION BY HASH({}) PARTITIONS {}",
        TABLEGROUP_TPCC, hashColumn, layout.PartitionCount);
}

std::vector<std::string> BuildCreateStatements(const TSchemaLayout& layout, const TObSchemaOptions& options) {
    const std::string fkStockWarehouse = options.EnableForeignKeys
        ? "    FOREIGN KEY (s_w_id) REFERENCES warehouse (w_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkStockItem = options.EnableForeignKeys
        ? "    FOREIGN KEY (s_i_id) REFERENCES item (i_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkDistrict = options.EnableForeignKeys
        ? "    FOREIGN KEY (d_w_id) REFERENCES warehouse (w_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkCustomer = options.EnableForeignKeys
        ? "    FOREIGN KEY (c_w_id, c_d_id) REFERENCES district (d_w_id, d_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkHistoryCustomer = options.EnableForeignKeys
        ? "    FOREIGN KEY (h_c_w_id, h_c_d_id, h_c_id) REFERENCES customer (c_w_id, c_d_id, c_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkHistoryDistrict = options.EnableForeignKeys
        ? "    FOREIGN KEY (h_w_id, h_d_id) REFERENCES district (d_w_id, d_id) ON DELETE CASCADE"
        : "";
    const std::string fkOorder = options.EnableForeignKeys
        ? "    FOREIGN KEY (o_w_id, o_d_id, o_c_id) REFERENCES customer (c_w_id, c_d_id, c_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkNewOrder = options.EnableForeignKeys
        ? "    FOREIGN KEY (no_w_id, no_d_id, no_o_id) REFERENCES oorder (o_w_id, o_d_id, o_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkOrderLineOorder = options.EnableForeignKeys
        ? "    FOREIGN KEY (ol_w_id, ol_d_id, ol_o_id) REFERENCES oorder (o_w_id, o_d_id, o_id) ON DELETE CASCADE,\n"
        : "";
    const std::string fkOrderLineStock = options.EnableForeignKeys
        ? "    FOREIGN KEY (ol_supply_w_id, ol_i_id) REFERENCES stock (s_w_id, s_i_id) ON DELETE CASCADE,\n"
        : "";

    const std::string wh = ClusterTableSuffix(layout, "w_id");
    const std::string dWh = ClusterTableSuffix(layout, "d_w_id");
    const std::string cWh = ClusterTableSuffix(layout, "c_w_id");
    const std::string hWh = ClusterTableSuffix(layout, "h_w_id");
    const std::string noWh = ClusterTableSuffix(layout, "no_w_id");
    const std::string oWh = ClusterTableSuffix(layout, "o_w_id");
    const std::string olWh = ClusterTableSuffix(layout, "ol_w_id");
    const std::string sWh = layout.UseClusterLayout
        ? fmt::format(
            " use_bloom_filter = true TABLEGROUP = {} PARTITION BY HASH(s_w_id) PARTITIONS {}",
            TABLEGROUP_TPCC, layout.PartitionCount)
        : std::string{};

    const std::string historyDataSuffix = options.EnableForeignKeys ? "," : "";
    const std::string historyHistId = layout.UseClusterLayout
        ? "    hist_id INT          NOT NULL AUTO_INCREMENT,\n"
        : "";
    const std::string historyPkClause = layout.UseClusterLayout
        ? ",\n    PRIMARY KEY (h_w_id, hist_id)"
        : "";

    return {
        fmt::format(R"(CREATE TABLE warehouse (
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
){})", wh),
        R"(CREATE TABLE item (
    i_id    int           NOT NULL,
    i_name  varchar(24)   NOT NULL,
    i_price decimal(5, 2) NOT NULL,
    i_data  varchar(50)   NOT NULL,
    i_im_id int           NOT NULL,
    PRIMARY KEY (i_id)
))",
        fmt::format(R"(CREATE TABLE stock (
    s_w_id       int           NOT NULL,
    s_i_id       int           NOT NULL,
    s_quantity   int           NOT NULL,
    s_ytd        decimal(8, 2) NOT NULL,
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
{}{}    PRIMARY KEY (s_w_id, s_i_id)
){})", fkStockWarehouse, fkStockItem, sWh),
        fmt::format(R"(CREATE TABLE district (
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
{}    PRIMARY KEY (d_w_id, d_id)
){})", fkDistrict, dWh),
        fmt::format(R"(CREATE TABLE customer (
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
{}    PRIMARY KEY (c_w_id, c_d_id, c_id)
){})", fkCustomer, cWh),
        fmt::format(R"(CREATE TABLE history (
{}    h_c_id   int           NOT NULL,
    h_c_d_id int           NOT NULL,
    h_c_w_id int           NOT NULL,
    h_d_id   int           NOT NULL,
    h_w_id   int           NOT NULL,
    h_date   timestamp     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    h_amount decimal(6, 2) NOT NULL,
    h_data   varchar(24)   NOT NULL{}
{}{}{}
){})", historyHistId, historyDataSuffix, fkHistoryCustomer, fkHistoryDistrict, historyPkClause, hWh),
        fmt::format(R"(CREATE TABLE oorder (
    o_w_id       int       NOT NULL,
    o_d_id       int       NOT NULL,
    o_id         int       NOT NULL,
    o_c_id       int       NOT NULL,
    o_carrier_id int                DEFAULT NULL,
    o_ol_cnt     int       NOT NULL,
    o_all_local  int       NOT NULL,
    o_entry_d    timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (o_w_id, o_d_id, o_id),
{}
    UNIQUE (o_w_id, o_d_id, o_c_id, o_id)
){})", fkOorder, oWh),
        fmt::format(R"(CREATE TABLE new_order (
    no_w_id int NOT NULL,
    no_d_id int NOT NULL,
    no_o_id int NOT NULL,
{}    PRIMARY KEY (no_w_id, no_d_id, no_o_id)
){})", fkNewOrder, noWh),
        fmt::format(R"(CREATE TABLE order_line (
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
{}{}    PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)
){})", fkOrderLineOorder, fkOrderLineStock, olWh),
    };
}

void CreateTableGroup(TObConnection& conn, const TSchemaLayout& layout) {
    if (!layout.UseClusterLayout) {
        return;
    }
    conn.ExecuteSimple(fmt::format(
        "CREATE TABLEGROUP IF NOT EXISTS {} binding true partition by hash partitions {}",
        TABLEGROUP_TPCC, layout.PartitionCount));
}

bool IndexExists(TObConnection& conn, const std::string& database, const std::string& indexName) {
    auto result = conn.Query(
        "SELECT 1 AS ok FROM information_schema.statistics "
        "WHERE table_schema = ? AND index_name = ? LIMIT 1",
        MakeParams(database, indexName));
    return result.TryNextRow();
}

} // namespace

void InitSync(
    const std::string& connectionString,
    const std::string& path,
    const TObSchemaOptions& options)
{
    LOG_I("Initializing TPC-C schema...");

    try {
        auto cfg = ConfigWithPath(connectionString, path);
        const std::string db = EffectiveDatabase(cfg);
        auto conn = ConnectToTargetDatabase(cfg);

        const TSchemaLayout layout = ResolveSchemaLayout(*conn, options);

        LOG_I("Using database '" << db << "'");
        LOG_I("Foreign keys: " << ForeignKeysModeLabel(options.EnableForeignKeys));
        if (layout.UseClusterLayout) {
            LOG_I("OceanBase cluster layout: TABLEGROUP=" << TABLEGROUP_TPCC
                  << ", HASH partitions=" << layout.PartitionCount);
        } else {
            LOG_I("Using non-partitioned schema");
        }

        ExecAll(*conn, std::vector<std::string>(std::begin(DROP_TABLES), std::end(DROP_TABLES)));
        CreateTableGroup(*conn, layout);
        ExecAll(*conn, BuildCreateStatements(layout, options));

        LOG_I("All TPC-C tables created successfully");
    } catch (const std::exception& e) {
        LOG_E("Failed to create TPC-C tables: " << e.what());
        LOG_E("After fixing the reason, you might need to run `tpcc clean`.");
        throw;
    }
}

void CreateIndexes(const std::string& connectionString, const std::string& path, bool useLocalIndexes) {
    LOG_I("Creating secondary indexes...");

    try {
        auto cfg = ConfigWithPath(connectionString, path);
        const std::string db = EffectiveDatabase(cfg);
        auto conn = ConnectToTargetDatabase(cfg);
        // Fresh session: raise ob_query_timeout via connection property query_timeout.
        conn->ConfigureBulkLoadSession();

        const bool localIndexes = useLocalIndexes || IsOceanBaseServer(*conn);
        const char* localSuffix = localIndexes ? " LOCAL" : "";

        const std::vector<std::pair<const char*, std::string>> indexes = {
            {INDEX_CUSTOMER_NAME,
             fmt::format(
                 "CREATE INDEX {} ON customer (c_w_id, c_d_id, c_last, c_first){}",
                 INDEX_CUSTOMER_NAME, localSuffix)},
            {INDEX_ORDER,
             fmt::format(
                 "CREATE INDEX {} ON oorder (o_w_id, o_d_id, o_c_id, o_id){}",
                 INDEX_ORDER, localSuffix)},
        };

        for (const auto& [name, sql] : indexes) {
            if (IndexExists(*conn, db, name)) {
                LOG_I("Index '" << name << "' already exists, skipping");
                continue;
            }
            try {
                conn->ExecuteSimple(sql);
                LOG_I("Created index '" << name << "'");
            } catch (const TObDbError& err) {
                if (err.Code() == 1061) {
                    LOG_I("Index '" << name << "' already exists, skipping");
                    continue;
                }
                throw;
            }
        }
        LOG_I("Secondary indexes ready");
    } catch (const std::exception& e) {
        LOG_E("Failed to create indexes: " << e.what());
        throw;
    }
}

void AnalyzeTables(const std::string& connectionString, const std::string& path) {
    LOG_I("Running ANALYZE TABLE on TPC-C tables...");
    auto conn = ConnectToTargetDatabase(ConfigWithPath(connectionString, path));
    // Fresh session: raise ob_query_timeout via connection property query_timeout.
    // ANALYZE on large customer/stock/order_line otherwise hits [4012] Timeout.
    conn->ConfigureBulkLoadSession();
    for (const auto* table : TPCC_TABLES) {
        LOG_I("Analyzing table `" << table << "`...");
        conn->QuerySimple(fmt::format("ANALYZE TABLE {}", QuoteIdent(table)));
    }
}

} // namespace NTpcc

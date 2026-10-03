#include "init.h"

#include "ob_connection.h"
#include "ob_errors.h"

#include <constants.h>
#include <log.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

void ExecAll(TObConnection& conn, const std::vector<std::string>& statements) {
    for (const auto& sql : statements) {
        conn.ExecuteSimple(sql);
    }
}

TObSchemaLayout ResolveSchemaLayout(TObConnection& conn, const TObSchemaOptions& options) {
    const int partitions = ResolveObPartitionCount(options);
    TObSchemaLayout layout;
    const bool oceanbase = IsOceanBaseServer(conn);
    if (oceanbase) {
        layout.DuplicateItem = true;
    }
    if (partitions < 0) {
        return layout;
    }
    if (!oceanbase) {
        if (options.PartitionCount > 0) {
            LOG_W("Ignoring OceanBase partitions: target is not OceanBase");
        }
        return layout;
    }
    layout.UseClusterLayout = true;
    layout.PartitionCount = partitions;
    return layout;
}

std::string ClusterTableSuffix(const TObSchemaLayout& layout, const char* hashColumn) {
    if (!layout.UseClusterLayout) {
        return {};
    }
    return fmt::format(
        " TABLEGROUP = {} PARTITION BY HASH({}) PARTITIONS {}",
        TABLEGROUP_TPCC, hashColumn, layout.PartitionCount);
}

std::string ItemTableSuffix(const TObSchemaLayout& layout) {
    if (!layout.DuplicateItem) {
        return {};
    }
    return " DUPLICATE_SCOPE = 'cluster'";
}

} // namespace

std::vector<std::string> BuildObCreateStatements(
    const TObSchemaLayout& layout,
    const TObSchemaOptions& options)
{
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
    const std::string item = ItemTableSuffix(layout);
    const std::string sWh = layout.UseClusterLayout
        ? fmt::format(
            " use_bloom_filter = true TABLEGROUP = {} PARTITION BY HASH(s_w_id) PARTITIONS {}",
            TABLEGROUP_TPCC, layout.PartitionCount)
        : std::string{};

    const std::string historyDataSuffix = options.EnableForeignKeys ? "," : "";
    // Signed INT stops at 2147483647 (~71582 warehouses of initial history).
    // OceanBase caps AUTO_INCREMENT at the column type; BIGINT matches
    // PostgreSQL bigint IDENTITY and YDB Int64.
    const std::string historyHistId = "    hist_id BIGINT       NOT NULL AUTO_INCREMENT,\n";
    const std::string historyPkClause = ",\n    PRIMARY KEY (h_w_id, hist_id)";

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
        fmt::format(R"(CREATE TABLE item (
    i_id    int           NOT NULL,
    i_name  varchar(24)   NOT NULL,
    i_price decimal(5, 2) NOT NULL,
    i_data  varchar(50)   NOT NULL,
    i_im_id int           NOT NULL,
    PRIMARY KEY (i_id)
){})", item),
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

namespace {

void CreateTableGroup(TObConnection& conn, const TObSchemaLayout& layout) {
    if (!layout.UseClusterLayout) {
        return;
    }
    conn.ExecuteSimple(fmt::format(
        "CREATE TABLEGROUP IF NOT EXISTS {} binding true partition by hash partitions {}",
        TABLEGROUP_TPCC, layout.PartitionCount));
}

bool IndexExists(TObConnection& conn, const std::string& database, const std::string& indexName) {
    auto result = conn.QuerySimple(
        "SELECT 1 AS ok FROM information_schema.statistics WHERE table_schema = "
        + QuoteSqlString(database) + " AND index_name = " + QuoteSqlString(indexName)
        + " LIMIT 1");
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

        const TObSchemaLayout layout = ResolveSchemaLayout(*conn, options);

        LOG_I("Using database '" << db << "'");
        LOG_I("Foreign keys: " << ForeignKeysModeLabel(options.EnableForeignKeys));
        if (layout.UseClusterLayout) {
            LOG_I("OceanBase cluster layout: TABLEGROUP=" << TABLEGROUP_TPCC
                  << ", HASH partitions=" << layout.PartitionCount);
        } else {
            LOG_I("Using non-partitioned schema");
        }
        if (layout.DuplicateItem) {
            LOG_I("item: DUPLICATE_SCOPE=cluster (replicated to every observer)");
        }

        ExecAll(*conn, std::vector<std::string>(std::begin(DROP_TABLES), std::end(DROP_TABLES)));
        CreateTableGroup(*conn, layout);
        ExecAll(*conn, BuildObCreateStatements(layout, options));

        LOG_I("All TPC-C tables created successfully");
    } catch (const std::exception& e) {
        LOG_E("Failed to create TPC-C tables: " << e.what());
        LOG_E("After fixing the reason, you might need to run `tpcc drop`.");
        throw;
    }
}

void CreateIndexes(
    const std::string& connectionString,
    const std::string& path,
    bool useLocalIndexes,
    int indexParallel)
{
    LOG_I("Creating secondary indexes...");

    try {
        auto cfg = ConfigWithPath(connectionString, path);
        const std::string db = EffectiveDatabase(cfg);
        auto conn = ConnectToTargetDatabase(cfg);
        // Fresh session: raise ob_query_timeout via connection property query_timeout.
        conn->ConfigureBulkLoadSession();

        TObSchemaOptions parallelOpts;
        parallelOpts.IndexParallel = indexParallel;
        indexParallel = ResolveObIndexParallel(parallelOpts);

        const bool oceanbase = IsOceanBaseServer(*conn);
        const bool localIndexes = useLocalIndexes || oceanbase;
        if (!oceanbase && indexParallel > 1) {
            LOG_W("Ignoring OceanBase index_parallel: target is not OceanBase");
        }
        std::string indexSuffix;
        if (localIndexes) {
            indexSuffix += " LOCAL";
        }
        if (oceanbase && indexParallel > 1) {
            indexSuffix += fmt::format(" PARALLEL {}", indexParallel);
            LOG_I("CREATE INDEX DOP: PARALLEL " << indexParallel);
        }

        const std::vector<std::pair<const char*, std::string>> indexes = {
            {INDEX_CUSTOMER_NAME,
             fmt::format(
                 "CREATE INDEX {} ON customer (c_w_id, c_d_id, c_last, c_first){}",
                 INDEX_CUSTOMER_NAME, indexSuffix)},
            {INDEX_ORDER,
             fmt::format(
                 "CREATE INDEX {} ON oorder (o_w_id, o_d_id, o_c_id, o_id){}",
                 INDEX_ORDER, indexSuffix)},
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

namespace {

constexpr const char* OB_STATS_METHOD_OPT = "FOR ALL COLUMNS SIZE 1";

const char* ObHashPartitionColumn(const char* table) {
    if (table == nullptr) {
        return nullptr;
    }
    if (std::strcmp(table, TABLE_WAREHOUSE) == 0) return "w_id";
    if (std::strcmp(table, TABLE_STOCK) == 0) return "s_w_id";
    if (std::strcmp(table, TABLE_DISTRICT) == 0) return "d_w_id";
    if (std::strcmp(table, TABLE_CUSTOMER) == 0) return "c_w_id";
    if (std::strcmp(table, TABLE_HISTORY) == 0) return "h_w_id";
    if (std::strcmp(table, TABLE_OORDER) == 0) return "o_w_id";
    if (std::strcmp(table, TABLE_NEW_ORDER) == 0) return "no_w_id";
    if (std::strcmp(table, TABLE_ORDER_LINE) == 0) return "ol_w_id";
    return nullptr;
}

bool SameColumnName(const std::string& left, const char* right) {
    if (right == nullptr || left.size() != std::strlen(right)) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(left[i]);
        const unsigned char b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

// Partition stats are already committed. Publish table-level row counts and
// column NDVs from them so the optimizer has global stats without a second
// full scan (that scan is what hits [4012] Timeout on stock / order_line).
void PublishObGlobalStatsFromPartitions(
    TObConnection& conn,
    const std::string& database,
    const char* table)
{
    const std::string where = " WHERE owner = " + QuoteSqlString(database)
        + " AND table_name = " + QuoteSqlString(table)
        + " AND partition_name IS NOT NULL";
    auto tableStats = conn.QuerySimple(
        "SELECT CAST(SUM(num_rows) AS SIGNED), "
        "CAST(SUM(avg_row_len * num_rows) / NULLIF(SUM(num_rows), 0) AS SIGNED) "
        "FROM oceanbase.DBA_TAB_STATISTICS" + where);
    if (!tableStats.TryNextRow()) {
        LOG_W("No partition statistics rows for `" << table << "`");
        return;
    }
    const auto numRows = tableStats.GetOptionalInt64(0);
    if (!numRows || *numRows < 0) {
        LOG_W("No partition row counts for `" << table << "`; global table stats not updated");
        return;
    }
    const auto avgLen = tableStats.GetOptionalInt64(1);
    std::string setTable = fmt::format(
        "CALL DBMS_STATS.SET_TABLE_STATS({}, {}, numrows=>{}",
        QuoteSqlString(database), QuoteSqlString(table), *numRows);
    if (avgLen && *avgLen > 0) {
        setTable += fmt::format(", avgrlen=>{}", *avgLen);
    }
    setTable += ")";
    conn.ExecuteSimple(setTable);
    LOG_I("Set global table stats for `" << table << "` (numrows=" << *numRows << ")");

    // Table stats are already committed. A column-view failure must not be
    // reported as a failure of the row-count publish above.
    try {
        const char* hashColumn = ObHashPartitionColumn(table);
        auto columns = conn.QuerySimple(BuildObPartitionColumnStatsQuery(database, table));
        int published = 0;
        while (columns.TryNextRow()) {
            const auto name = columns.GetOptionalString(0);
            const auto sumNdv = columns.GetOptionalInt64(1);
            const auto maxNdv = columns.GetOptionalInt64(2);
            if (!name || name->empty() || !maxNdv || *maxNdv < 0) {
                continue;
            }
            const bool partitionKey = SameColumnName(*name, hashColumn);
            const int64_t ndv = (partitionKey && sumNdv && *sumNdv >= 0) ? *sumNdv : *maxNdv;
            std::string setColumn = fmt::format(
                "CALL DBMS_STATS.SET_COLUMN_STATS({}, {}, {}, distcnt=>{}",
                QuoteSqlString(database), QuoteSqlString(table), QuoteSqlString(*name), ndv);
            const auto nulls = columns.GetOptionalInt64(3);
            if (nulls && *nulls >= 0) {
                setColumn += fmt::format(", nullcnt=>{}", *nulls);
            }
            const auto avgColLen = columns.GetOptionalInt64(4);
            if (avgColLen && *avgColLen > 0) {
                setColumn += fmt::format(", avgclen=>{}", *avgColLen);
            }
            setColumn += ")";
            conn.ExecuteSimple(setColumn);
            ++published;
        }
        LOG_I("Set global column stats for `" << table << "` (" << published << " columns)");
    } catch (const std::exception& e) {
        LOG_W("Could not publish global column stats for `" << table << "` (" << e.what()
              << "); global row counts and partition statistics are in place");
    }
}

} // namespace

std::vector<TObGatherTableStatsCall> BuildObGatherTableStatsCalls(
    const std::string& database,
    const char* table,
    int degree,
    int hashPartitions)
{
    if (table == nullptr || table[0] == '\0') {
        throw std::runtime_error("table name must not be empty");
    }
    if (degree < 1) {
        throw std::runtime_error("analyze degree must be a positive integer");
    }
    const std::string db = QuoteSqlString(database);
    const std::string tab = QuoteSqlString(table);
    const std::string methodOpt = QuoteSqlString(OB_STATS_METHOD_OPT);
    const bool perPartition = hashPartitions >= 1 && std::strcmp(table, TABLE_ITEM) != 0;
    if (!perPartition) {
        TObGatherTableStatsCall call;
        call.Label = "`" + std::string(table) + "`";
        call.Sql = fmt::format(
            "CALL DBMS_STATS.GATHER_TABLE_STATS({}, {}, degree=>{}, method_opt=>{})",
            db, tab, degree, methodOpt);
        return {std::move(call)};
    }

    std::vector<TObGatherTableStatsCall> calls;
    calls.reserve(static_cast<size_t>(hashPartitions));
    const std::string granularity = QuoteSqlString("PARTITION");
    for (int i = 0; i < hashPartitions; ++i) {
        const std::string part = "p" + std::to_string(i);
        TObGatherTableStatsCall call;
        call.Label = "`" + std::string(table) + "` partition " + part
            + " (" + std::to_string(i + 1) + "/" + std::to_string(hashPartitions) + ")";
        // degree 1: this partition's leader is one observer. Parallelism is
        // the concurrent calls in ExecuteGatherCalls, not PX inside the partition.
        call.Sql = fmt::format(
            "CALL DBMS_STATS.GATHER_TABLE_STATS({}, {}, {}, degree=>1, granularity=>{}, method_opt=>{})",
            db, tab, QuoteSqlString(part), granularity, methodOpt);
        calls.push_back(std::move(call));
    }
    return calls;
}

int ObStatsGatherSessionCount(int hashPartitions) {
    if (hashPartitions < 1) {
        return 1;
    }
    return std::min(hashPartitions, OB_MAX_PARALLEL_STATS_GATHERS);
}

std::string BuildObPartitionColumnStatsQuery(
    const std::string& database,
    const char* table)
{
    if (table == nullptr || table[0] == '\0') {
        throw std::runtime_error("table name must not be empty");
    }
    return "SELECT column_name, "
        "CAST(SUM(num_distinct) AS SIGNED), "
        "CAST(MAX(num_distinct) AS SIGNED), "
        "CAST(SUM(num_nulls) AS SIGNED), "
        "CAST(AVG(avg_col_len) AS SIGNED) "
        "FROM oceanbase.DBA_PART_COL_STATISTICS"
        " WHERE owner = " + QuoteSqlString(database)
        + " AND table_name = " + QuoteSqlString(table)
        + " AND partition_name IS NOT NULL"
        " GROUP BY column_name";
}

namespace {

void ExecuteGatherCalls(
    TObConnection& primary,
    const TObConnectionConfig& cfg,
    const std::vector<TObGatherTableStatsCall>& calls,
    int sessions)
{
    if (calls.empty()) {
        return;
    }
    const int workers = std::max(1, std::min(sessions, static_cast<int>(calls.size())));
    if (workers == 1) {
        for (const auto& call : calls) {
            LOG_I("Gathering stats for " << call.Label << "...");
            primary.ExecuteSimple(call.Sql);
        }
        return;
    }

    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex errorMu;
    std::exception_ptr error;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&, cfg]() {
            try {
                auto conn = ConnectToTargetDatabase(cfg);
                conn->ConfigureBulkLoadSession();
                for (;;) {
                    if (failed.load(std::memory_order_relaxed)) {
                        return;
                    }
                    const size_t idx = next.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= calls.size()) {
                        return;
                    }
                    LOG_I("Gathering stats for " << calls[idx].Label << "...");
                    conn->ExecuteSimple(calls[idx].Sql);
                }
            } catch (const std::exception& ex) {
                LOG_E("Stats gather failed: " << ex.what());
                bool expected = false;
                if (failed.compare_exchange_strong(expected, true)) {
                    std::lock_guard lock(errorMu);
                    error = std::current_exception();
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

} // namespace

void AnalyzeTables(
    const std::string& connectionString,
    const std::string& path,
    const TObSchemaOptions& options)
{
    auto cfg = ConfigWithPath(connectionString, path);
    const std::string db = EffectiveDatabase(cfg);
    auto conn = ConnectToTargetDatabase(cfg);
    // Fresh session: raise ob_query_timeout via connection property query_timeout.
    // HASH partitions are gathered concurrently, each with degree 1, so a
    // stock/order_line scan stays inside ob_query_timeout and on its own leader.
    conn->ConfigureBulkLoadSession();

    if (!IsOceanBaseServer(*conn)) {
        LOG_I("Running ANALYZE TABLE on TPC-C tables...");
        for (const auto* table : TPCC_TABLES) {
            LOG_I("Analyzing table `" << table << "`...");
            conn->QuerySimple(fmt::format("ANALYZE TABLE {}", QuoteIdent(table)));
        }
        return;
    }

    const int partitions = ResolveObPartitionCount(options);
    const int degree = ResolveObAnalyzeDegree(options);
    const int sessions = ObStatsGatherSessionCount(partitions);
    LOG_I("Gathering optimizer statistics via DBMS_STATS (parallel_sessions=" << sessions
          << ", partition_degree=1, method_opt=FOR ALL COLUMNS SIZE 1)...");
    for (const auto* table : TPCC_TABLES) {
        const auto calls = BuildObGatherTableStatsCalls(db, table, degree, partitions);
        ExecuteGatherCalls(*conn, cfg, calls, sessions);
        const bool perPartition = partitions >= 1 && std::strcmp(table, TABLE_ITEM) != 0;
        if (!perPartition) {
            continue;
        }
        try {
            LOG_I("Publishing global stats for `" << table << "` from partition statistics...");
            PublishObGlobalStatsFromPartitions(*conn, db, table);
        } catch (const std::exception& e) {
            LOG_W("Could not publish global stats for `" << table << "` (" << e.what()
                  << "); partition statistics are in place");
        }
    }
}

} // namespace NTpcc

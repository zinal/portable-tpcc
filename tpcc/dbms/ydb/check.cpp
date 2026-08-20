#include "check.h"

#include "path_checker.h"
#include "run_config.h"

#include <artifacts.h>
#include <catalog.h>
#include <constants.h>
#include <log.h>
#include <report.h>

#include <fmt/format.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/params/params.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace NTpcc {

namespace {

std::string PhaseName(ECheckPhase phase) {
    return phase == ECheckPhase::AfterImport ? "after-import" : "after-run";
}

void AddResult(TCheckReport& report, TCheckResult result) {
    switch (result.Status) {
        case ECheckStatus::Passed:
            ++report.PassedCount;
            break;
        case ECheckStatus::Failed:
            ++report.FailedCount;
            break;
        case ECheckStatus::Skipped:
            ++report.SkippedCount;
            break;
        case ECheckStatus::Error:
            ++report.ErrorCount;
            break;
    }
    report.Results.push_back(std::move(result));
}

// Zero money literal matching the Decimal(22,9) physical type.
constexpr const char* kZeroMoney = "CAST('0.00' AS Decimal(22,9))";

// Heavy consistency checks scan order_line/customer; run them in warehouse
// ranges (same chunk sizes as PostgreSQL / OceanBase) to stay under YDB
// transaction memory limits at large scale.
constexpr int kRangeWide = 50;
constexpr int kRangeNarrow = 10;

using TQueryMap = std::unordered_map<std::string, std::vector<std::string>>;

void AddSingle(TQueryMap& q, const std::string& id, std::string sql) {
    q[id].push_back(std::move(sql));
}

template <typename F>
void AddRanged(TQueryMap& q, const std::string& id, int warehouses, int rangeSize, F&& makeSql) {
    for (int startWh = 1; startWh <= warehouses; startWh += rangeSize) {
        const int endWh = std::min(startWh + rangeSize - 1, warehouses);
        q[id].push_back(makeSql(startWh, endWh));
    }
}

TQueryMap BuildQueries(int warehouses) {
    const int64_t customers = static_cast<int64_t>(warehouses) * DISTRICT_COUNT * CUSTOMERS_PER_DISTRICT;
    const int64_t stock = static_cast<int64_t>(warehouses) * ITEM_COUNT;
    const int64_t newOrders = static_cast<int64_t>(warehouses) * DISTRICT_COUNT *
        (CUSTOMERS_PER_DISTRICT - FIRST_UNPROCESSED_O_ID + 1);
    const int64_t districts = static_cast<int64_t>(warehouses) * DISTRICT_COUNT;
    const int deliveredOrdersPerDistrict = FIRST_UNPROCESSED_O_ID - 1;

    const std::string warehouseYtd = WAREHOUSE_INITIAL_YTD.ToString();
    const std::string districtYtd = DISTRICT_INITIAL_YTD.ToString();

    TQueryMap q;
    // Cardinality + id ranges (same strength as PostgreSQL / OceanBase base checks).
    AddSingle(q, "cardinality.warehouse", fmt::format(
        "SELECT (COUNT(*) = {0} AND MIN(w_id) = 1 AND MAX(w_id) = {0}) AS ok FROM `warehouse`;",
        warehouses));
    AddSingle(q, "cardinality.district", fmt::format(
        "SELECT (COUNT(*) = {0} AND MIN(d_w_id) = 1 AND MAX(d_w_id) = {1} "
        "AND MIN(d_id) = {2} AND MAX(d_id) = {3}) AS ok FROM `district`;",
        districts, warehouses, DISTRICT_LOW_ID, DISTRICT_HIGH_ID));
    AddSingle(q, "cardinality.customer", fmt::format(
        "SELECT (COUNT(*) = {0} AND MIN(c_w_id) = 1 AND MAX(c_w_id) = {1} "
        "AND MIN(c_d_id) = {2} AND MAX(c_d_id) = {3} "
        "AND MIN(c_id) = 1 AND MAX(c_id) = {4}) AS ok FROM `customer`;",
        customers, warehouses, DISTRICT_LOW_ID, DISTRICT_HIGH_ID, CUSTOMERS_PER_DISTRICT));
    AddSingle(q, "cardinality.item", fmt::format(
        "SELECT (COUNT(*) = {0} AND MIN(i_id) = 1 AND MAX(i_id) = {0}) AS ok FROM `item`;",
        ITEM_COUNT));
    AddSingle(q, "cardinality.stock", fmt::format(
        "SELECT (COUNT(*) = {0} AND COUNT(DISTINCT s_w_id) = {1} "
        "AND MIN(s_w_id) = 1 AND MAX(s_w_id) = {1} "
        "AND MIN(s_i_id) = 1 AND MAX(s_i_id) = {2}) AS ok FROM `stock`;",
        stock, warehouses, ITEM_COUNT));
    AddSingle(q, "cardinality.oorder", fmt::format(
        "SELECT (COUNT(*) = {0} AND MIN(o_w_id) = 1 AND MAX(o_w_id) = {1} "
        "AND MIN(o_d_id) = {2} AND MAX(o_d_id) = {3} "
        "AND MIN(o_id) = 1 AND MAX(o_id) = {4}) AS ok FROM `oorder`;",
        customers, warehouses, DISTRICT_LOW_ID, DISTRICT_HIGH_ID, CUSTOMERS_PER_DISTRICT));
    AddSingle(q, "cardinality.new_order", fmt::format(
        "SELECT (COUNT(*) = {0} AND MIN(no_w_id) = 1 AND MAX(no_w_id) = {1} "
        "AND MIN(no_d_id) = {2} AND MAX(no_d_id) = {3} "
        "AND MIN(no_o_id) >= {4} AND MAX(no_o_id) = {5}) AS ok FROM `new_order`;",
        newOrders, warehouses, DISTRICT_LOW_ID, DISTRICT_HIGH_ID,
        FIRST_UNPROCESSED_O_ID, CUSTOMERS_PER_DISTRICT));
    AddSingle(q, "cardinality.history", fmt::format(
        "SELECT (COUNT(*) = {0} AND MIN(h_c_w_id) = 1 AND MAX(h_c_w_id) = {1}) AS ok FROM `history`;",
        customers, warehouses));
    // Every district must have exactly CUSTOMERS_PER_DISTRICT distinct order ids in order_line.
    AddSingle(q, "cardinality.order_line", fmt::format(R"(
        SELECT COUNT(*) = {} AS ok FROM (
            SELECT ol_w_id, ol_d_id
              FROM `order_line`
             GROUP BY ol_w_id, ol_d_id
            HAVING COUNT(DISTINCT ol_o_id) = {}
        ) AS good;
    )", districts, CUSTOMERS_PER_DISTRICT));

    AddSingle(q, "post_import.d_next_o_id", fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `district` WHERE d_next_o_id != {};", CUSTOMERS_PER_DISTRICT + 1));
    AddSingle(q, "post_import.w_ytd", fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `warehouse` WHERE w_ytd != CAST('{}' AS Decimal(22,9));",
        warehouseYtd));
    AddSingle(q, "post_import.d_ytd", fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `district` WHERE d_ytd != CAST('{}' AS Decimal(22,9));",
        districtYtd));
    AddSingle(q, "post_import.o_carrier_id", fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `oorder` WHERE o_id >= {} AND o_carrier_id IS NOT NULL;",
        FIRST_UNPROCESSED_O_ID));
    AddSingle(q, "post_import.o_carrier_id_range", fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `oorder` WHERE o_id < {} AND (o_carrier_id IS NULL OR o_carrier_id < 1 OR o_carrier_id > 10);",
        FIRST_UNPROCESSED_O_ID));
    AddSingle(q, "post_import.ol_delivery_d", fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `order_line` WHERE ol_o_id >= {} AND ol_delivery_d IS NOT NULL;",
        FIRST_UNPROCESSED_O_ID));
    AddSingle(q, "post_import.ol_amount_delivered", fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `order_line` WHERE ol_o_id < {} AND ol_amount != {};",
        FIRST_UNPROCESSED_O_ID, kZeroMoney));
    // NULL delivery on a delivered order is a failure (match PG IS DISTINCT FROM semantics).
    AddSingle(q, "post_import.ol_delivery_eq_entry", fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok
          FROM `order_line` AS ol
          INNER JOIN `oorder` AS o
             ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id AND o.o_id = ol.ol_o_id
         WHERE ol.ol_o_id < {}
           AND (ol.ol_delivery_d IS NULL OR ol.ol_delivery_d != o.o_entry_d);
    )", FIRST_UNPROCESSED_O_ID));

    // TPC-C §3.3.2.1: W_YTD = sum(D_YTD) per warehouse.
    AddSingle(q, "consistency.3.3.2.1", fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT w.w_id
              FROM `warehouse` AS w
              LEFT JOIN (
                  SELECT d_w_id, SUM(d_ytd) AS sum_d_ytd
                    FROM `district`
                   GROUP BY d_w_id
              ) AS d ON w.w_id = d.d_w_id
             WHERE w.w_ytd != COALESCE(d.sum_d_ytd, {})
        ) AS bad;
    )", kZeroMoney));

    // TPC-C §3.3.2.2: D_NEXT_O_ID - 1 = max(O_ID); when new-orders exist also = max(NO_O_ID).
    AddSingle(q, "consistency.3.3.2.2", R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT d.d_w_id, d.d_id
              FROM `district` AS d
              LEFT JOIN (
                  SELECT o_w_id, o_d_id, MAX(o_id) AS max_o_id
                    FROM `oorder`
                   GROUP BY o_w_id, o_d_id
              ) AS o ON d.d_w_id = o.o_w_id AND d.d_id = o.o_d_id
              LEFT JOIN (
                  SELECT no_w_id, no_d_id, MAX(no_o_id) AS max_no_o_id
                    FROM `new_order`
                   GROUP BY no_w_id, no_d_id
              ) AS n ON d.d_w_id = n.no_w_id AND d.d_id = n.no_d_id
             WHERE o.max_o_id IS NULL
                OR (d.d_next_o_id - 1) != o.max_o_id
                OR (n.max_no_o_id IS NOT NULL AND n.max_no_o_id != o.max_o_id)
        ) AS bad;
    )");

    AddSingle(q, "consistency.3.3.2.3", R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT no_w_id, no_d_id
              FROM `new_order`
             GROUP BY no_w_id, no_d_id
            HAVING COUNT(*) != MAX(no_o_id) - MIN(no_o_id) + 1
        ) AS bad;
    )");

    // TPC-C §3.3.2.4: sum(O_OL_CNT) = count(order_line) per district.
    AddRanged(q, "consistency.3.3.2.4", warehouses, kRangeWide,
        [](int startWh, int endWh) {
            return fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT o.o_w_id, o.o_d_id
              FROM (
                  SELECT o_w_id, o_d_id, SUM(o_ol_cnt) AS sum_ol_cnt
                    FROM `oorder`
                   WHERE o_w_id >= {0} AND o_w_id <= {1}
                   GROUP BY o_w_id, o_d_id
              ) AS o
              FULL JOIN (
                  SELECT ol_w_id, ol_d_id, COUNT(*) AS ol_count
                    FROM `order_line`
                   WHERE ol_w_id >= {0} AND ol_w_id <= {1}
                   GROUP BY ol_w_id, ol_d_id
              ) AS ol ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id
             WHERE COALESCE(o.sum_ol_cnt, 0) != COALESCE(ol.ol_count, 0)
        ) AS bad;
    )", startWh, endWh);
        });

    AddRanged(q, "consistency.3.3.2.5", warehouses, kRangeWide,
        [](int startWh, int endWh) {
            return fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT no.no_w_id AS w, no.no_d_id AS d, no.no_o_id AS o
              FROM `new_order` AS no
              LEFT JOIN `oorder` AS oo
                ON oo.o_w_id = no.no_w_id AND oo.o_d_id = no.no_d_id AND oo.o_id = no.no_o_id
             WHERE no.no_w_id >= {0} AND no.no_w_id <= {1}
               AND (oo.o_id IS NULL OR oo.o_carrier_id IS NOT NULL)
            UNION ALL
            SELECT oo.o_w_id AS w, oo.o_d_id AS d, oo.o_id AS o
              FROM `oorder` AS oo
              LEFT JOIN `new_order` AS no
                ON oo.o_w_id = no.no_w_id AND oo.o_d_id = no.no_d_id AND oo.o_id = no.no_o_id
             WHERE oo.o_w_id >= {0} AND oo.o_w_id <= {1}
               AND oo.o_carrier_id IS NULL AND no.no_o_id IS NULL
        ) AS bad;
    )", startWh, endWh);
        });

    // TPC-C §3.3.2.6: each order's O_OL_CNT matches line count; every line has a parent order.
    AddRanged(q, "consistency.3.3.2.6", warehouses, kRangeWide,
        [](int startWh, int endWh) {
            return fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT o.o_w_id, o.o_d_id, o.o_id
              FROM `oorder` AS o
              LEFT JOIN (
                  SELECT ol_w_id, ol_d_id, ol_o_id, COUNT(*) AS cnt
                    FROM `order_line`
                   WHERE ol_w_id >= {0} AND ol_w_id <= {1}
                   GROUP BY ol_w_id, ol_d_id, ol_o_id
              ) AS l ON o.o_w_id = l.ol_w_id AND o.o_d_id = l.ol_d_id AND o.o_id = l.ol_o_id
             WHERE o.o_w_id >= {0} AND o.o_w_id <= {1}
               AND o.o_ol_cnt != COALESCE(l.cnt, 0)
            UNION ALL
            SELECT l2.ol_w_id, l2.ol_d_id, l2.ol_o_id
              FROM (
                  SELECT DISTINCT ol_w_id, ol_d_id, ol_o_id
                    FROM `order_line`
                   WHERE ol_w_id >= {0} AND ol_w_id <= {1}
              ) AS l2
              LEFT JOIN `oorder` AS o2
                ON l2.ol_w_id = o2.o_w_id AND l2.ol_d_id = o2.o_d_id AND l2.ol_o_id = o2.o_id
             WHERE o2.o_id IS NULL
        ) AS bad;
    )", startWh, endWh);
        });

    AddRanged(q, "consistency.3.3.2.7", warehouses, kRangeNarrow,
        [](int startWh, int endWh) {
            return fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok
          FROM `oorder` AS o
          INNER JOIN `order_line` AS ol
             ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id AND o.o_id = ol.ol_o_id
         WHERE o.o_w_id >= {0} AND o.o_w_id <= {1}
           AND ((o.o_carrier_id IS NULL AND ol.ol_delivery_d IS NOT NULL)
            OR (o.o_carrier_id IS NOT NULL AND ol.ol_delivery_d IS NULL));
    )", startWh, endWh);
        });

    // TPC-C §3.3.2.8: W_YTD = sum(H_AMOUNT) per warehouse.
    AddSingle(q, "consistency.3.3.2.8", fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT w.w_id
              FROM `warehouse` AS w
              LEFT JOIN (
                  SELECT h_w_id, SUM(h_amount) AS sum_h
                    FROM `history`
                   GROUP BY h_w_id
              ) AS h ON w.w_id = h.h_w_id
             WHERE w.w_ytd != COALESCE(h.sum_h, {})
        ) AS bad;
    )", kZeroMoney));

    // TPC-C §3.3.2.9: D_YTD = sum(H_AMOUNT) per district.
    AddSingle(q, "consistency.3.3.2.9", fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT d.d_w_id, d.d_id
              FROM `district` AS d
              LEFT JOIN (
                  SELECT h_w_id, h_d_id, SUM(h_amount) AS sum_h
                    FROM `history`
                   GROUP BY h_w_id, h_d_id
              ) AS h ON d.d_w_id = h.h_w_id AND d.d_id = h.h_d_id
             WHERE d.d_ytd != COALESCE(h.sum_h, {})
        ) AS bad;
    )", kZeroMoney));

    // TPC-C §3.3.2.10: C_BALANCE = sum(delivered OL_AMOUNT) - sum(H_AMOUNT).
    // CAST money aggregates / arithmetic back to Decimal(22,9): YQL widens
    // Decimal on SUM/+/- and rejects implicit narrowing in COALESCE/!=.
    AddRanged(q, "consistency.3.3.2.10", warehouses, kRangeNarrow,
        [](int startWh, int endWh) {
            return fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT c.c_w_id, c.c_d_id, c.c_id
              FROM `customer` AS c
              LEFT JOIN (
                  SELECT o.o_w_id AS w_id, o.o_d_id AS d_id, o.o_c_id AS c_id,
                         CAST(SUM(ol.ol_amount) AS Decimal(22,9)) AS ol_sum
                    FROM `oorder` AS o
                    JOIN `order_line` AS ol
                      ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id
                   WHERE ol.ol_delivery_d IS NOT NULL
                     AND o.o_w_id >= {0} AND o.o_w_id <= {1}
                   GROUP BY o.o_w_id, o.o_d_id, o.o_c_id
              ) AS ols ON c.c_w_id = ols.w_id AND c.c_d_id = ols.d_id AND c.c_id = ols.c_id
              LEFT JOIN (
                  SELECT h_c_w_id, h_c_d_id, h_c_id,
                         CAST(SUM(h_amount) AS Decimal(22,9)) AS h_sum
                    FROM `history`
                   WHERE h_c_w_id >= {0} AND h_c_w_id <= {1}
                   GROUP BY h_c_w_id, h_c_d_id, h_c_id
              ) AS hs ON c.c_w_id = hs.h_c_w_id AND c.c_d_id = hs.h_c_d_id AND c.c_id = hs.h_c_id
             WHERE c.c_w_id >= {0} AND c.c_w_id <= {1}
               AND c.c_balance != CAST(
                     COALESCE(ols.ol_sum, {2}) - COALESCE(hs.h_sum, {2}) AS Decimal(22,9))
        ) AS bad;
    )", startWh, endWh, kZeroMoney);
        });

    // TPC-C §3.3.2.11 (after import): orders - new_orders = FIRST_UNPROCESSED_O_ID - 1 per district.
    AddRanged(q, "consistency.3.3.2.11", warehouses, kRangeWide,
        [deliveredOrdersPerDistrict](int startWh, int endWh) {
            return fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT COALESCE(o.o_w_id, n.no_w_id) AS w_id,
                   COALESCE(o.o_d_id, n.no_d_id) AS d_id
              FROM (
                  SELECT o_w_id, o_d_id, COUNT(*) AS order_cnt
                    FROM `oorder`
                   WHERE o_w_id >= {0} AND o_w_id <= {1}
                   GROUP BY o_w_id, o_d_id
              ) AS o
              FULL JOIN (
                  SELECT no_w_id, no_d_id, COUNT(*) AS new_order_cnt
                    FROM `new_order`
                   WHERE no_w_id >= {0} AND no_w_id <= {1}
                   GROUP BY no_w_id, no_d_id
              ) AS n ON o.o_w_id = n.no_w_id AND o.o_d_id = n.no_d_id
             WHERE (COALESCE(o.order_cnt, 0) - COALESCE(n.new_order_cnt, 0)) != {2}
        ) AS bad;
    )", startWh, endWh, deliveredOrdersPerDistrict);
        });

    // TPC-C §3.3.2.12: C_BALANCE + C_YTD_PAYMENT = sum(delivered OL_AMOUNT).
    AddRanged(q, "consistency.3.3.2.12", warehouses, kRangeNarrow,
        [](int startWh, int endWh) {
            return fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT c.c_w_id, c.c_d_id, c.c_id
              FROM `customer` AS c
              LEFT JOIN (
                  SELECT o.o_w_id AS w_id, o.o_d_id AS d_id, o.o_c_id AS c_id,
                         CAST(SUM(ol.ol_amount) AS Decimal(22,9)) AS ol_sum
                    FROM `oorder` AS o
                    JOIN `order_line` AS ol
                      ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id
                   WHERE ol.ol_delivery_d IS NOT NULL
                     AND o.o_w_id >= {0} AND o.o_w_id <= {1}
                   GROUP BY o.o_w_id, o.o_d_id, o.o_c_id
              ) AS l ON c.c_w_id = l.w_id AND c.c_d_id = l.d_id AND c.c_id = l.c_id
             WHERE c.c_w_id >= {0} AND c.c_w_id <= {1}
               AND CAST(c.c_balance + c.c_ytd_payment AS Decimal(22,9))
                   != COALESCE(l.ol_sum, {2})
        ) AS bad;
    )", startWh, endWh, kZeroMoney);
        });

    return q;
}

bool QueryBool(TYdbConnection& connection, const std::string& query) {
    // TablePathPrefix requires the absolute path including the database name.
    const std::string sql = fmt::format(
        "PRAGMA TablePathPrefix(\"{}\");\n{}",
        connection.AbsolutePathPrefix(),
        query);
    auto result = connection.QueryClient().RetryQuery([&](NYdb::NQuery::TSession session) {
        return session.ExecuteQuery(sql, NYdb::NQuery::TTxControl::NoTx());
    }).GetValueSync();
    if (!result.IsSuccess()) {
        throw std::runtime_error(result.GetIssues().ToOneLineString());
    }
    NYdb::TResultSetParser parser(result.GetResultSet(0));
    if (!parser.TryNextRow()) {
        throw std::runtime_error("check query returned no rows");
    }
    // YQL types comparisons involving MIN/MAX as Optional<Bool>; plain
    // COUNT(*) comparisons stay Bool. Accept both.
    auto& okParser = parser.ColumnParser("ok");
    if (okParser.GetKind() == NYdb::TTypeParser::ETypeKind::Optional) {
        const auto ok = okParser.GetOptionalBool();
        if (!ok.has_value()) {
            throw std::runtime_error("check query returned null ok");
        }
        return *ok;
    }
    return okParser.GetBool();
}

// Runs one or more query chunks for a single catalog check. All chunks must
// return ok=true. When concurrency > 1, warehouse-range chunks are executed
// in parallel against the shared QueryClient (thread-safe).
void RunQueryChunks(
    TYdbConnection& connection,
    const std::vector<std::string>& queries,
    int concurrency,
    TCheckResult& result,
    bool isCardinality,
    bool& baseFailed)
{
    if (queries.empty()) {
        result.Status = ECheckStatus::Skipped;
        result.Detail = "not implemented for YDB yet";
        return;
    }

    const size_t workers = concurrency <= 1
        ? 1
        : std::min(static_cast<size_t>(concurrency), queries.size());

    if (workers == 1) {
        try {
            for (size_t i = 0; i < queries.size(); ++i) {
                if (!QueryBool(connection, queries[i])) {
                    result.Status = ECheckStatus::Failed;
                    result.Detail = queries.size() == 1
                        ? "query returned false"
                        : fmt::format("query returned false (chunk {}/{})", i + 1, queries.size());
                    if (isCardinality) {
                        baseFailed = true;
                    }
                    return;
                }
            }
            result.Status = ECheckStatus::Passed;
        } catch (const std::exception& ex) {
            result.Status = ECheckStatus::Error;
            result.Detail = ex.what();
            if (isCardinality) {
                baseFailed = true;
            }
        }
        return;
    }

    std::atomic<bool> failed{false};
    std::atomic<bool> errored{false};
    std::mutex detailMutex;
    std::string detail;
    std::atomic<size_t> next{0};

    auto workerFn = [&]() {
        for (;;) {
            if (failed.load(std::memory_order_relaxed) ||
                errored.load(std::memory_order_relaxed)) {
                return;
            }
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= queries.size()) {
                return;
            }
            try {
                if (!QueryBool(connection, queries[i])) {
                    bool expected = false;
                    if (failed.compare_exchange_strong(expected, true)) {
                        std::lock_guard lock(detailMutex);
                        detail = fmt::format(
                            "query returned false (chunk {}/{})", i + 1, queries.size());
                    }
                    return;
                }
            } catch (const std::exception& ex) {
                bool expected = false;
                if (errored.compare_exchange_strong(expected, true)) {
                    std::lock_guard lock(detailMutex);
                    detail = ex.what();
                }
                return;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (size_t i = 0; i < workers; ++i) {
        threads.emplace_back(workerFn);
    }
    for (auto& t : threads) {
        t.join();
    }

    if (errored.load()) {
        result.Status = ECheckStatus::Error;
        result.Detail = detail;
        if (isCardinality) {
            baseFailed = true;
        }
    } else if (failed.load()) {
        result.Status = ECheckStatus::Failed;
        result.Detail = detail;
        if (isCardinality) {
            baseFailed = true;
        }
    } else {
        result.Status = ECheckStatus::Passed;
    }
}

} // anonymous

TCheckReport RunYdbChecks(const TYdbConnectionConfig& connectionConfig, const TCheckRequest& request) {
    TCheckReport report;
    report.RunId = request.RunId;
    report.Instance = request.Instance;
    report.Phase = PhaseName(request.Phase);
    report.WarehouseCount = request.WarehouseCount;

    TYdbConnection connection(connectionConfig);
    auto queries = BuildQueries(request.WarehouseCount);
    const int concurrency = request.CheckConcurrency <= 1 ? 1 : request.CheckConcurrency;
    if (concurrency > 1) {
        LOG_I("Running YDB checks with concurrency=" << concurrency);
    }

    // Match PG/OceanBase: abort consistency/post-import suite if base cardinality failed.
    bool baseFailed = false;
    for (const auto& entry : CheckCatalog()) {
        if (!CheckAppliesToPhase(entry.Phase, request.Phase)) {
            continue;
        }

        TCheckResult result;
        result.Id = entry.Id;
        result.Title = entry.Title;
        const bool isCardinality = std::string_view(entry.Id).rfind("cardinality.", 0) == 0;

        if (baseFailed && !isCardinality) {
            result.Status = ECheckStatus::Skipped;
            result.Detail = "skipped: base cardinality failed";
            AddResult(report, std::move(result));
            continue;
        }

        auto it = queries.find(std::string(entry.Id));
        if (it == queries.end()) {
            result.Status = ECheckStatus::Skipped;
            result.Detail = "not implemented for YDB yet";
            AddResult(report, std::move(result));
            continue;
        }

        RunQueryChunks(connection, it->second, concurrency, result, isCardinality, baseFailed);
        AddResult(report, std::move(result));
    }

    return report;
}

void CheckSync(
    const TYdbConnectionConfig& connectionConfig,
    int warehouseCount,
    bool afterImport,
    int checkConcurrency)
{
    TCheckRequest request;
    request.WarehouseCount = warehouseCount;
    request.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterRun;
    request.CheckConcurrency = checkConcurrency <= 1 ? 1 : checkConcurrency;
    auto report = RunYdbChecks(connectionConfig, request);
    for (const auto& r : report.Results) {
        std::cout << r.Id << ": " << CheckStatusToString(r.Status);
        if (!r.Detail.empty()) {
            std::cout << " (" << r.Detail << ")";
        }
        std::cout << "\n";
    }
    if (!report.Ok()) {
        throw std::runtime_error("YDB TPC-C checks failed");
    }
}

TYdbCheckAdapter::TYdbCheckAdapter(TYdbConnectionConfig connectionConfig)
    : ConnectionConfig_(std::move(connectionConfig))
{}

TCheckReport TYdbCheckAdapter::Run(const TCheckRequest& request) {
    return RunYdbChecks(ConnectionConfig_, request);
}

int RunCheckFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    bool afterImport,
    bool afterRun,
    int checkConcurrency)
{
    if (afterImport == afterRun) {
        throw std::runtime_error("check requires exactly one of --after-import or --after-run");
    }

    const auto doc = LoadRunConfigDocument(runConfigPath);
    const std::string instanceDir = InstanceWorkDir(doc, "check", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();
    WriteProcessJson(paths, doc, instance, "check", static_cast<int>(::getpid()), nonce);

    int exitCode = 1;
    try {
        const auto connection = BuildYdbConnectionConfig(doc);
        CheckDbForRun(connection, doc.ScaleWarehouses);

        TCheckRequest request;
        request.RunId = doc.RunId;
        request.Instance = instance;
        request.WarehouseCount = doc.ScaleWarehouses;
        request.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterRun;
        request.CheckConcurrency = checkConcurrency <= 1 ? 1 : checkConcurrency;

        TYdbCheckAdapter adapter(connection);
        const auto report = adapter.Run(request);

        const std::string checksDir = doc.RunDir + "/checks";
        std::filesystem::create_directories(checksDir);
        const std::string reportPath = checksDir + "/" + report.Phase + ".json";
        WriteCheckReportJson(reportPath, report);
        LOG_I("Check report written to " << reportPath);
        exitCode = report.Ok() ? 0 : 1;
    } catch (const std::exception& ex) {
        LOG_E("Check failed: " << ex.what());
        exitCode = 1;
    }

    WriteArtifactManifest(paths, instance, nonce, exitCode);
    return exitCode;
}

} // namespace NTpcc

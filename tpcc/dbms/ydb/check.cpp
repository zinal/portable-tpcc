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

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <unistd.h>

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

std::string CardinalityQuery(const std::string& table, int64_t expected) {
    return fmt::format("SELECT COUNT(*) = {} AS ok FROM `{}`;", expected, table);
}

// Zero money literal matching the Decimal(22,9) physical type.
constexpr const char* kZeroMoney = "CAST('0.00' AS Decimal(22,9))";

std::unordered_map<std::string, std::string> BuildQueries(int warehouses) {
    const int64_t customers = static_cast<int64_t>(warehouses) * DISTRICT_COUNT * CUSTOMERS_PER_DISTRICT;
    const int64_t stock = static_cast<int64_t>(warehouses) * ITEM_COUNT;
    const int64_t newOrders = static_cast<int64_t>(warehouses) * DISTRICT_COUNT *
        (CUSTOMERS_PER_DISTRICT - FIRST_UNPROCESSED_O_ID + 1);
    const int64_t districts = static_cast<int64_t>(warehouses) * DISTRICT_COUNT;
    const int deliveredOrdersPerDistrict = FIRST_UNPROCESSED_O_ID - 1;

    const std::string warehouseYtd = WAREHOUSE_INITIAL_YTD.ToString();
    const std::string districtYtd = DISTRICT_INITIAL_YTD.ToString();

    std::unordered_map<std::string, std::string> q;
    q["cardinality.warehouse"] = CardinalityQuery(TABLE_WAREHOUSE, warehouses);
    q["cardinality.district"] = CardinalityQuery(TABLE_DISTRICT, districts);
    q["cardinality.customer"] = CardinalityQuery(TABLE_CUSTOMER, customers);
    q["cardinality.item"] = CardinalityQuery(TABLE_ITEM, ITEM_COUNT);
    q["cardinality.stock"] = CardinalityQuery(TABLE_STOCK, stock);
    q["cardinality.oorder"] = CardinalityQuery(TABLE_OORDER, customers);
    q["cardinality.new_order"] = CardinalityQuery(TABLE_NEW_ORDER, newOrders);
    q["cardinality.history"] = CardinalityQuery(TABLE_HISTORY, customers);
    // Every district must have exactly CUSTOMERS_PER_DISTRICT distinct order ids in order_line.
    q["cardinality.order_line"] = fmt::format(R"(
        SELECT COUNT(*) = {} AS ok FROM (
            SELECT ol_w_id, ol_d_id
              FROM `order_line`
             GROUP BY ol_w_id, ol_d_id
            HAVING COUNT(DISTINCT ol_o_id) = {}
        ) AS good;
    )", districts, CUSTOMERS_PER_DISTRICT);

    q["post_import.d_next_o_id"] = fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `district` WHERE d_next_o_id != {};", CUSTOMERS_PER_DISTRICT + 1);
    q["post_import.w_ytd"] = fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `warehouse` WHERE w_ytd != CAST('{}' AS Decimal(22,9));",
        warehouseYtd);
    q["post_import.d_ytd"] = fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `district` WHERE d_ytd != CAST('{}' AS Decimal(22,9));",
        districtYtd);
    q["post_import.o_carrier_id"] = fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `oorder` WHERE o_id >= {} AND o_carrier_id IS NOT NULL;",
        FIRST_UNPROCESSED_O_ID);
    q["post_import.o_carrier_id_range"] = fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `oorder` WHERE o_id < {} AND (o_carrier_id IS NULL OR o_carrier_id < 1 OR o_carrier_id > 10);",
        FIRST_UNPROCESSED_O_ID);
    q["post_import.ol_delivery_d"] = fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `order_line` WHERE ol_o_id >= {} AND ol_delivery_d IS NOT NULL;",
        FIRST_UNPROCESSED_O_ID);
    q["post_import.ol_amount_delivered"] = fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `order_line` WHERE ol_o_id < {} AND ol_amount != {};",
        FIRST_UNPROCESSED_O_ID, kZeroMoney);
    // NULL delivery on a delivered order is a failure (match PG IS DISTINCT FROM semantics).
    q["post_import.ol_delivery_eq_entry"] = fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok
          FROM `order_line` AS ol
          INNER JOIN `oorder` AS o
             ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id AND o.o_id = ol.ol_o_id
         WHERE ol.ol_o_id < {}
           AND (ol.ol_delivery_d IS NULL OR ol.ol_delivery_d != o.o_entry_d);
    )", FIRST_UNPROCESSED_O_ID);

    // TPC-C §3.3.2.1: W_YTD = sum(D_YTD) per warehouse.
    q["consistency.3.3.2.1"] = fmt::format(R"(
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
    )", kZeroMoney);

    // TPC-C §3.3.2.2: D_NEXT_O_ID - 1 = max(O_ID); when new-orders exist also = max(NO_O_ID).
    q["consistency.3.3.2.2"] = R"(
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
    )";

    q["consistency.3.3.2.3"] = R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT no_w_id, no_d_id
              FROM `new_order`
             GROUP BY no_w_id, no_d_id
            HAVING COUNT(*) != MAX(no_o_id) - MIN(no_o_id) + 1
        ) AS bad;
    )";

    // TPC-C §3.3.2.4: sum(O_OL_CNT) = count(order_line) per district.
    q["consistency.3.3.2.4"] = R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT o.o_w_id, o.o_d_id
              FROM (
                  SELECT o_w_id, o_d_id, SUM(o_ol_cnt) AS sum_ol_cnt
                    FROM `oorder`
                   GROUP BY o_w_id, o_d_id
              ) AS o
              FULL JOIN (
                  SELECT ol_w_id, ol_d_id, COUNT(*) AS ol_count
                    FROM `order_line`
                   GROUP BY ol_w_id, ol_d_id
              ) AS ol ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id
             WHERE COALESCE(o.sum_ol_cnt, 0) != COALESCE(ol.ol_count, 0)
        ) AS bad;
    )";

    q["consistency.3.3.2.5"] = R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT no.no_w_id AS w, no.no_d_id AS d, no.no_o_id AS o
              FROM `new_order` AS no
              LEFT JOIN `oorder` AS oo
                ON oo.o_w_id = no.no_w_id AND oo.o_d_id = no.no_d_id AND oo.o_id = no.no_o_id
             WHERE oo.o_id IS NULL OR oo.o_carrier_id IS NOT NULL
            UNION ALL
            SELECT oo.o_w_id AS w, oo.o_d_id AS d, oo.o_id AS o
              FROM `oorder` AS oo
              LEFT JOIN `new_order` AS no
                ON oo.o_w_id = no.no_w_id AND oo.o_d_id = no.no_d_id AND oo.o_id = no.no_o_id
             WHERE oo.o_carrier_id IS NULL AND no.no_o_id IS NULL
        ) AS bad;
    )";

    // TPC-C §3.3.2.6: each order's O_OL_CNT matches line count; every line has a parent order.
    q["consistency.3.3.2.6"] = R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT o.o_w_id, o.o_d_id, o.o_id
              FROM `oorder` AS o
              LEFT JOIN (
                  SELECT ol_w_id, ol_d_id, ol_o_id, COUNT(*) AS cnt
                    FROM `order_line`
                   GROUP BY ol_w_id, ol_d_id, ol_o_id
              ) AS l ON o.o_w_id = l.ol_w_id AND o.o_d_id = l.ol_d_id AND o.o_id = l.ol_o_id
             WHERE o.o_ol_cnt != COALESCE(l.cnt, 0)
            UNION ALL
            SELECT l2.ol_w_id, l2.ol_d_id, l2.ol_o_id
              FROM (
                  SELECT DISTINCT ol_w_id, ol_d_id, ol_o_id
                    FROM `order_line`
              ) AS l2
              LEFT JOIN `oorder` AS o2
                ON l2.ol_w_id = o2.o_w_id AND l2.ol_d_id = o2.o_d_id AND l2.ol_o_id = o2.o_id
             WHERE o2.o_id IS NULL
        ) AS bad;
    )";

    q["consistency.3.3.2.7"] = R"(
        SELECT COUNT(*) = 0 AS ok
          FROM `oorder` AS o
          INNER JOIN `order_line` AS ol
             ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id AND o.o_id = ol.ol_o_id
         WHERE (o.o_carrier_id IS NULL AND ol.ol_delivery_d IS NOT NULL)
            OR (o.o_carrier_id IS NOT NULL AND ol.ol_delivery_d IS NULL);
    )";

    // TPC-C §3.3.2.8: W_YTD = sum(H_AMOUNT) per warehouse.
    q["consistency.3.3.2.8"] = fmt::format(R"(
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
    )", kZeroMoney);

    // TPC-C §3.3.2.9: D_YTD = sum(H_AMOUNT) per district.
    q["consistency.3.3.2.9"] = fmt::format(R"(
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
    )", kZeroMoney);

    // TPC-C §3.3.2.10: C_BALANCE = sum(delivered OL_AMOUNT) - sum(H_AMOUNT).
    q["consistency.3.3.2.10"] = fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT c.c_w_id, c.c_d_id, c.c_id
              FROM `customer` AS c
              LEFT JOIN (
                  SELECT o.o_w_id AS w_id, o.o_d_id AS d_id, o.o_c_id AS c_id,
                         SUM(ol.ol_amount) AS ol_sum
                    FROM `oorder` AS o
                    JOIN `order_line` AS ol
                      ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id
                   WHERE ol.ol_delivery_d IS NOT NULL
                   GROUP BY o.o_w_id, o.o_d_id, o.o_c_id
              ) AS ols ON c.c_w_id = ols.w_id AND c.c_d_id = ols.d_id AND c.c_id = ols.c_id
              LEFT JOIN (
                  SELECT h_c_w_id, h_c_d_id, h_c_id, SUM(h_amount) AS h_sum
                    FROM `history`
                   GROUP BY h_c_w_id, h_c_d_id, h_c_id
              ) AS hs ON c.c_w_id = hs.h_c_w_id AND c.c_d_id = hs.h_c_d_id AND c.c_id = hs.h_c_id
             WHERE c.c_balance != (COALESCE(ols.ol_sum, {}) - COALESCE(hs.h_sum, {}))
        ) AS bad;
    )", kZeroMoney, kZeroMoney);

    // TPC-C §3.3.2.11 (after import): orders - new_orders = FIRST_UNPROCESSED_O_ID - 1 per district.
    q["consistency.3.3.2.11"] = fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT COALESCE(o.o_w_id, n.no_w_id) AS w_id,
                   COALESCE(o.o_d_id, n.no_d_id) AS d_id
              FROM (
                  SELECT o_w_id, o_d_id, COUNT(*) AS order_cnt
                    FROM `oorder`
                   GROUP BY o_w_id, o_d_id
              ) AS o
              FULL JOIN (
                  SELECT no_w_id, no_d_id, COUNT(*) AS new_order_cnt
                    FROM `new_order`
                   GROUP BY no_w_id, no_d_id
              ) AS n ON o.o_w_id = n.no_w_id AND o.o_d_id = n.no_d_id
             WHERE (COALESCE(o.order_cnt, 0) - COALESCE(n.new_order_cnt, 0)) != {}
        ) AS bad;
    )", deliveredOrdersPerDistrict);

    // TPC-C §3.3.2.12: C_BALANCE + C_YTD_PAYMENT = sum(delivered OL_AMOUNT).
    q["consistency.3.3.2.12"] = fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT c.c_w_id, c.c_d_id, c.c_id
              FROM `customer` AS c
              LEFT JOIN (
                  SELECT o.o_w_id AS w_id, o.o_d_id AS d_id, o.o_c_id AS c_id,
                         SUM(ol.ol_amount) AS ol_sum
                    FROM `oorder` AS o
                    JOIN `order_line` AS ol
                      ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id
                   WHERE ol.ol_delivery_d IS NOT NULL
                   GROUP BY o.o_w_id, o.o_d_id, o.o_c_id
              ) AS l ON c.c_w_id = l.w_id AND c.c_d_id = l.d_id AND c.c_id = l.c_id
             WHERE (c.c_balance + c.c_ytd_payment) != COALESCE(l.ol_sum, {})
        ) AS bad;
    )", kZeroMoney);

    return q;
}

bool QueryBool(TYdbConnection& connection, const std::string& query) {
    const std::string sql = fmt::format("PRAGMA TablePathPrefix(\"{}\");\n{}", connection.Config().Path, query);
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
    return parser.ColumnParser("ok").GetBool();
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

    for (const auto& entry : CheckCatalog()) {
        if (!CheckAppliesToPhase(entry.Phase, request.Phase)) {
            continue;
        }

        TCheckResult result;
        result.Id = entry.Id;
        result.Title = entry.Title;
        auto it = queries.find(std::string(entry.Id));
        if (it == queries.end()) {
            result.Status = ECheckStatus::Skipped;
            result.Detail = "not implemented for YDB yet";
            AddResult(report, std::move(result));
            continue;
        }

        try {
            if (QueryBool(connection, it->second)) {
                result.Status = ECheckStatus::Passed;
            } else {
                result.Status = ECheckStatus::Failed;
                result.Detail = "query returned false";
            }
        } catch (const std::exception& ex) {
            result.Status = ECheckStatus::Error;
            result.Detail = ex.what();
        }
        AddResult(report, std::move(result));
    }

    return report;
}

void CheckSync(const TYdbConnectionConfig& connectionConfig, int warehouseCount, bool afterImport) {
    TCheckRequest request;
    request.WarehouseCount = warehouseCount;
    request.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterRun;
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
    bool afterRun)
{
    if (afterImport == afterRun) {
        throw std::runtime_error("check requires exactly one of --after-import or --after-run");
    }

    const auto doc = LoadRunConfigDocument(runConfigPath);
    const auto connection = BuildYdbConnectionConfig(doc);
    CheckDbForRun(connection, doc.ScaleWarehouses);

    TCheckRequest request;
    request.RunId = doc.RunId;
    request.Instance = instance;
    request.WarehouseCount = doc.ScaleWarehouses;
    request.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterRun;

    TYdbCheckAdapter adapter(connection);
    const auto report = adapter.Run(request);

    const std::string checksDir = doc.RunDir + "/checks";
    std::filesystem::create_directories(checksDir);
    const std::string reportPath = checksDir + "/" + report.Phase + ".json";
    WriteCheckReportJson(reportPath, report);

    const std::string instanceDir = InstanceWorkDir(doc, "check", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();
    WriteProcessJson(paths, doc, instance, "check", static_cast<int>(::getpid()), nonce);
    WriteArtifactManifest(paths, instance, nonce, report.Ok() ? 0 : 1);

    LOG_I("Check report written to " << reportPath);
    return report.Ok() ? 0 : 1;
}

} // namespace NTpcc

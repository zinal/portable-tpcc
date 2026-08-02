#include "check.h"

#include "run_config.h"

#include <catalog.h>
#include <constants.h>
#include <log.h>
#include <report.h>

#include <fmt/format.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/params/params.h>

#include <iostream>
#include <stdexcept>
#include <unordered_map>

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

std::unordered_map<std::string, std::string> BuildQueries(int warehouses) {
    const int64_t customers = static_cast<int64_t>(warehouses) * DISTRICT_COUNT * CUSTOMERS_PER_DISTRICT;
    const int64_t stock = static_cast<int64_t>(warehouses) * ITEM_COUNT;
    const int64_t newOrders = static_cast<int64_t>(warehouses) * DISTRICT_COUNT *
        (CUSTOMERS_PER_DISTRICT - FIRST_UNPROCESSED_O_ID + 1);

    std::unordered_map<std::string, std::string> q;
    q["cardinality.warehouse"] = CardinalityQuery(TABLE_WAREHOUSE, warehouses);
    q["cardinality.district"] = CardinalityQuery(TABLE_DISTRICT, warehouses * DISTRICT_COUNT);
    q["cardinality.customer"] = CardinalityQuery(TABLE_CUSTOMER, customers);
    q["cardinality.item"] = CardinalityQuery(TABLE_ITEM, ITEM_COUNT);
    q["cardinality.stock"] = CardinalityQuery(TABLE_STOCK, stock);
    q["cardinality.oorder"] = CardinalityQuery(TABLE_OORDER, customers);
    q["cardinality.new_order"] = CardinalityQuery(TABLE_NEW_ORDER, newOrders);
    q["cardinality.history"] = CardinalityQuery(TABLE_HISTORY, customers);

    q["post_import.d_next_o_id"] = fmt::format(
        "SELECT COUNT(*) = 0 AS ok FROM `district` WHERE d_next_o_id != {};", CUSTOMERS_PER_DISTRICT + 1);
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
        "SELECT COUNT(*) = 0 AS ok FROM `order_line` WHERE ol_o_id < {} AND ol_amount != CAST('0.00' AS Decimal(22,9));",
        FIRST_UNPROCESSED_O_ID);
    q["post_import.ol_delivery_eq_entry"] = fmt::format(R"(
        SELECT COUNT(*) = 0 AS ok
          FROM `order_line` AS ol
          INNER JOIN `oorder` AS o
             ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id AND o.o_id = ol.ol_o_id
         WHERE ol.ol_o_id < {} AND ol.ol_delivery_d != o.o_entry_d;
    )", FIRST_UNPROCESSED_O_ID);

    q["consistency.3.3.2.3"] = R"(
        SELECT COUNT(*) = 0 AS ok FROM (
            SELECT no_w_id, no_d_id
              FROM `new_order`
             GROUP BY no_w_id, no_d_id
            HAVING COUNT(*) != MAX(no_o_id) - MIN(no_o_id) + 1
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
    q["consistency.3.3.2.7"] = R"(
        SELECT COUNT(*) = 0 AS ok
          FROM `oorder` AS o
          INNER JOIN `order_line` AS ol
             ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id AND o.o_id = ol.ol_o_id
         WHERE (o.o_carrier_id IS NULL AND ol.ol_delivery_d IS NOT NULL)
            OR (o.o_carrier_id IS NOT NULL AND ol.ol_delivery_d IS NULL);
    )";

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
    auto doc = LoadRunConfigDocument(runConfigPath);
    (void)instance;
    TCheckRequest request;
    request.RunId = doc.RunId;
    request.Instance = instance;
    request.WarehouseCount = doc.ScaleWarehouses;
    request.Phase = afterImport || !afterRun ? ECheckPhase::AfterImport : ECheckPhase::AfterRun;
    auto report = RunYdbChecks(BuildYdbConnectionConfig(doc), request);
    WriteCheckReportJson(InstanceWorkDir(doc, "check", instance) + "/checks-" + report.Phase + ".json", report);
    return report.Ok() ? 0 : 1;
}

} // namespace NTpcc

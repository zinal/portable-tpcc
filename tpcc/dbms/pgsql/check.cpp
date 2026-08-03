#include "check.h"

#include <artifacts.h>
#include "path_checker.h"
#include "run_config.h"

#include <report.h>

#include <constants.h>
#include <log.h>

#include <pqxx/pqxx>

#include <fmt/format.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace NTpcc {

namespace {

void CheckNoRows(pqxx::nontransaction& txn, const std::string& sql, const std::string& description = {}) {
    auto result = txn.exec(sql);
    if (!result.empty()) {
        throw std::runtime_error(
            description.empty() ? "Unexpected rows returned" : description);
    }
}

//-----------------------------------------------------------------------------

void BaseCheckWarehouseTable(pqxx::nontransaction& txn, int expectedWhNumber) {
    auto r = txn.exec(fmt::format(
        "SELECT COUNT(*) AS count, MAX(W_ID) AS max, MIN(W_ID) AS min FROM {}", TABLE_WAREHOUSE)).front();

    auto rowCount = r[0].as<int64_t>();
    auto maxWh = r[1].as<int>();
    auto minWh = r[2].as<int>();

    if (rowCount != expectedWhNumber || minWh != 1 || maxWh != expectedWhNumber) {
        throw std::runtime_error(fmt::format(
            "Inconsistent {}: count={}, min={}, max={}, expected={}",
            TABLE_WAREHOUSE, rowCount, minWh, maxWh, expectedWhNumber));
    }
}

void BaseCheckDistrictTable(pqxx::nontransaction& txn, int expectedWhNumber) {
    auto r = txn.exec(fmt::format(
        "SELECT COUNT(*) AS count, "
        "MAX(D_W_ID) AS max_w_id, MIN(D_W_ID) AS min_w_id, "
        "MAX(D_ID) AS max_d_id, MIN(D_ID) AS min_d_id "
        "FROM {}", TABLE_DISTRICT)).front();

    int expectedCount = expectedWhNumber * DISTRICT_COUNT;
    auto rowCount = r[0].as<int64_t>();
    if (rowCount != expectedCount)
        throw std::runtime_error(fmt::format("District count is {} and not {}", rowCount, expectedCount));

    auto maxWh = r[1].as<int>(), minWh = r[2].as<int>();
    auto maxDist = r[3].as<int>(), minDist = r[4].as<int>();

    if (minWh != 1 || maxWh != expectedWhNumber)
        throw std::runtime_error(fmt::format("District warehouse range [{}, {}] instead of [1, {}]", minWh, maxWh, expectedWhNumber));
    if (minDist != DISTRICT_LOW_ID || maxDist != DISTRICT_HIGH_ID)
        throw std::runtime_error(fmt::format("District ID range [{}, {}] instead of [{}, {}]", minDist, maxDist, DISTRICT_LOW_ID, DISTRICT_HIGH_ID));
}

void BaseCheckCustomerTable(pqxx::nontransaction& txn, int expectedWhNumber) {
    auto r = txn.exec(fmt::format(
        "SELECT COUNT(*) AS count, "
        "MAX(C_W_ID), MIN(C_W_ID), MAX(C_D_ID), MIN(C_D_ID), MAX(C_ID), MIN(C_ID) "
        "FROM {}", TABLE_CUSTOMER)).front();

    int expectedCount = expectedWhNumber * CUSTOMERS_PER_DISTRICT * DISTRICT_COUNT;
    auto rowCount = r[0].as<int64_t>();
    if (rowCount != expectedCount)
        throw std::runtime_error(fmt::format("Customer count is {} and not {}", rowCount, expectedCount));

    if (r[2].as<int>() != 1 || r[1].as<int>() != expectedWhNumber)
        throw std::runtime_error(fmt::format("Customer warehouse range [{}, {}] instead of [1, {}]", r[2].as<int>(), r[1].as<int>(), expectedWhNumber));
    if (r[4].as<int>() != DISTRICT_LOW_ID || r[3].as<int>() != DISTRICT_HIGH_ID)
        throw std::runtime_error("Customer district range mismatch");
    if (r[6].as<int>() != 1 || r[5].as<int>() != CUSTOMERS_PER_DISTRICT)
        throw std::runtime_error("Customer ID range mismatch");
}

void BaseCheckItemTable(pqxx::nontransaction& txn) {
    auto r = txn.exec(fmt::format(
        "SELECT COUNT(*), MAX(I_ID), MIN(I_ID) FROM {}", TABLE_ITEM)).front();

    auto rowCount = r[0].as<int64_t>();
    if (rowCount != ITEM_COUNT)
        throw std::runtime_error(fmt::format("Item count is {} and not {}", rowCount, ITEM_COUNT));
    if (r[2].as<int>() != 1 || r[1].as<int>() != ITEM_COUNT)
        throw std::runtime_error("Item ID range mismatch");
}

void BaseCheckStockTable(pqxx::nontransaction& txn, int expectedWhNumber) {
    auto r = txn.exec(fmt::format(
        "SELECT COUNT(*), COUNT(DISTINCT S_W_ID), MAX(S_W_ID), MIN(S_W_ID), MAX(S_I_ID), MIN(S_I_ID) "
        "FROM {}", TABLE_STOCK)).front();

    int expectedCount = expectedWhNumber * ITEM_COUNT;
    auto rowCount = r[0].as<int64_t>();
    if (rowCount != expectedCount)
        throw std::runtime_error(fmt::format("Stock count is {} and not {}", rowCount, expectedCount));

    auto whCount = r[1].as<int>();
    if (whCount != expectedWhNumber)
        throw std::runtime_error(fmt::format("Stock warehouse count is {} and not {}", whCount, expectedWhNumber));
    if (r[3].as<int>() != 1 || r[2].as<int>() != expectedWhNumber)
        throw std::runtime_error("Stock warehouse range mismatch");
    if (r[5].as<int>() != 1 || r[4].as<int>() != ITEM_COUNT)
        throw std::runtime_error("Stock item range mismatch");
}

void BaseCheckOorderTable(pqxx::nontransaction& txn, int expectedWhNumber) {
    auto r = txn.exec(fmt::format(
        "SELECT COUNT(*), MAX(O_W_ID), MIN(O_W_ID), MAX(O_D_ID), MIN(O_D_ID), MAX(O_ID), MIN(O_ID) "
        "FROM {}", TABLE_OORDER)).front();

    int expectedCount = expectedWhNumber * CUSTOMERS_PER_DISTRICT * DISTRICT_COUNT;
    auto rowCount = r[0].as<int64_t>();
    if (rowCount != expectedCount)
        throw std::runtime_error(fmt::format("Order count is {} and not {}", rowCount, expectedCount));

    if (r[2].as<int>() != 1 || r[1].as<int>() != expectedWhNumber)
        throw std::runtime_error("Order warehouse range mismatch");
    if (r[4].as<int>() != DISTRICT_LOW_ID || r[3].as<int>() != DISTRICT_HIGH_ID)
        throw std::runtime_error("Order district range mismatch");
    if (r[6].as<int>() != 1 || r[5].as<int>() != CUSTOMERS_PER_DISTRICT)
        throw std::runtime_error("Order ID range mismatch");
}

void BaseCheckNewOrderTable(pqxx::nontransaction& txn, int expectedWhNumber) {
    auto r = txn.exec(fmt::format(
        "SELECT COUNT(*), MAX(NO_W_ID), MIN(NO_W_ID), MAX(NO_D_ID), MIN(NO_D_ID), MAX(NO_O_ID), MIN(NO_O_ID) "
        "FROM {}", TABLE_NEW_ORDER)).front();

    const auto newOrdersPerDistrict = CUSTOMERS_PER_DISTRICT - FIRST_UNPROCESSED_O_ID + 1;
    int expectedCount = expectedWhNumber * newOrdersPerDistrict * DISTRICT_COUNT;
    auto rowCount = r[0].as<int64_t>();
    if (rowCount != expectedCount)
        throw std::runtime_error(fmt::format("New order count is {} and not {}", rowCount, expectedCount));

    if (r[2].as<int>() != 1 || r[1].as<int>() != expectedWhNumber)
        throw std::runtime_error("New order warehouse range mismatch");
    if (r[4].as<int>() != DISTRICT_LOW_ID || r[3].as<int>() != DISTRICT_HIGH_ID)
        throw std::runtime_error("New order district range mismatch");
    if (r[6].as<int>() < FIRST_UNPROCESSED_O_ID || r[5].as<int>() != CUSTOMERS_PER_DISTRICT)
        throw std::runtime_error("New order ID range mismatch");
}

void BaseCheckOrderLineTable(pqxx::nontransaction& txn, int expectedWhNumber) {
    auto r = txn.exec(fmt::format(
        "SELECT MIN(order_count) AS min_orders, MAX(order_count) AS max_orders, COUNT(*) AS district_count "
        "FROM ("
        "  SELECT OL_W_ID, OL_D_ID, COUNT(DISTINCT OL_O_ID) AS order_count "
        "  FROM {} GROUP BY OL_W_ID, OL_D_ID"
        ") sub", TABLE_ORDER_LINE)).front();

    int expectedDistrictCount = expectedWhNumber * DISTRICT_COUNT;
    auto districtCount = r[2].as<int64_t>();
    if (districtCount != expectedDistrictCount)
        throw std::runtime_error(fmt::format("Order line district count is {} and not {}", districtCount, expectedDistrictCount));

    auto minOrders = r[0].as<int64_t>();
    auto maxOrders = r[1].as<int64_t>();
    if (minOrders != CUSTOMERS_PER_DISTRICT || maxOrders != CUSTOMERS_PER_DISTRICT)
        throw std::runtime_error(fmt::format("Order line orders per district [{}, {}] instead of [{}, {}]",
            minOrders, maxOrders, CUSTOMERS_PER_DISTRICT, CUSTOMERS_PER_DISTRICT));
}

void BaseCheckHistoryTable(pqxx::nontransaction& txn, int expectedWhNumber) {
    auto r = txn.exec(fmt::format(
        "SELECT COUNT(*), MAX(H_C_W_ID), MIN(H_C_W_ID) FROM {}", TABLE_HISTORY)).front();

    int expectedCount = expectedWhNumber * CUSTOMERS_PER_DISTRICT * DISTRICT_COUNT;
    auto rowCount = r[0].as<int64_t>();
    if (rowCount != expectedCount)
        throw std::runtime_error(fmt::format("History count is {} and not {}", rowCount, expectedCount));
    if (r[2].as<int>() != 1 || r[1].as<int>() != expectedWhNumber)
        throw std::runtime_error("History warehouse range mismatch");
}

//-----------------------------------------------------------------------------
// Consistency checks based on TPC-C spec section 3.3.2
//-----------------------------------------------------------------------------

void ConsistencyCheck3321(pqxx::nontransaction& txn) {
    // W_YTD = sum(D_YTD) for each warehouse (LEFT JOIN catches empty districts)
    std::string sql = fmt::format(
        "SELECT w.W_ID, w.W_YTD, d.sum_d_ytd "
        "FROM {} AS w "
        "LEFT JOIN (SELECT D_W_ID, SUM(D_YTD) AS sum_d_ytd FROM {} GROUP BY D_W_ID) AS d "
        "ON w.W_ID = d.D_W_ID "
        "WHERE w.W_YTD IS DISTINCT FROM COALESCE(d.sum_d_ytd, 0) LIMIT 1",
        TABLE_WAREHOUSE, TABLE_DISTRICT);
    CheckNoRows(txn, sql);
}

void ConsistencyCheck3322(pqxx::nontransaction& txn) {
    // D_NEXT_O_ID - 1 = max(O_ID); when new-orders exist, also = max(NO_O_ID).
    // IS DISTINCT FROM so NULL aggregates do not fail open.
    std::string sql = fmt::format(
        "SELECT d.D_W_ID, d.D_ID, d.D_NEXT_O_ID, o.max_o_id, n.max_no_o_id "
        "FROM {} AS d "
        "LEFT JOIN (SELECT O_W_ID, O_D_ID, MAX(O_ID) AS max_o_id FROM {} GROUP BY O_W_ID, O_D_ID) AS o "
        "  ON d.D_W_ID = o.O_W_ID AND d.D_ID = o.O_D_ID "
        "LEFT JOIN (SELECT NO_W_ID, NO_D_ID, MAX(NO_O_ID) AS max_no_o_id FROM {} GROUP BY NO_W_ID, NO_D_ID) AS n "
        "  ON d.D_W_ID = n.NO_W_ID AND d.D_ID = n.NO_D_ID "
        "WHERE o.max_o_id IS NULL "
        "   OR (d.D_NEXT_O_ID - 1) IS DISTINCT FROM o.max_o_id "
        "   OR (n.max_no_o_id IS NOT NULL AND n.max_no_o_id IS DISTINCT FROM o.max_o_id) "
        "LIMIT 1",
        TABLE_DISTRICT, TABLE_OORDER, TABLE_NEW_ORDER);
    CheckNoRows(txn, sql);
}

void ConsistencyCheck3323(pqxx::nontransaction& txn) {
    // max(NO_O_ID) - min(NO_O_ID) + 1 = count(*) for each warehouse/district
    std::string sql = fmt::format(
        "SELECT NO_W_ID, NO_D_ID, COUNT(*) - (MAX(NO_O_ID) - MIN(NO_O_ID) + 1) AS delta "
        "FROM {} GROUP BY NO_W_ID, NO_D_ID "
        "HAVING COUNT(*) - (MAX(NO_O_ID) - MIN(NO_O_ID) + 1) != 0 LIMIT 1",
        TABLE_NEW_ORDER);
    CheckNoRows(txn, sql);
}

void ConsistencyCheck3324(pqxx::nontransaction& txn, int warehouseCount) {
    // sum(O_OL_CNT) = count of order_lines per district
    const int RANGE_SIZE = 50;
    for (int startWh = 1; startWh <= warehouseCount; startWh += RANGE_SIZE) {
        int endWh = std::min(startWh + RANGE_SIZE - 1, warehouseCount);
        std::string sql = fmt::format(
            "SELECT o.O_W_ID, o.O_D_ID, o.sum_ol_cnt, ol.ol_count "
            "FROM (SELECT O_W_ID, O_D_ID, SUM(O_OL_CNT) AS sum_ol_cnt "
            "      FROM {} WHERE O_W_ID >= {} AND O_W_ID <= {} GROUP BY O_W_ID, O_D_ID) AS o "
            "FULL JOIN (SELECT OL_W_ID, OL_D_ID, COUNT(*) AS ol_count "
            "           FROM {} WHERE OL_W_ID >= {} AND OL_W_ID <= {} GROUP BY OL_W_ID, OL_D_ID) AS ol "
            "  ON o.O_W_ID = ol.OL_W_ID AND o.O_D_ID = ol.OL_D_ID "
            "WHERE o.sum_ol_cnt IS DISTINCT FROM ol.ol_count LIMIT 1",
            TABLE_OORDER, startWh, endWh, TABLE_ORDER_LINE, startWh, endWh);
        CheckNoRows(txn, sql, fmt::format("3.3.2.4 w_id [{},{}]", startWh, endWh));
    }
}

void ConsistencyCheck3325(pqxx::nontransaction& txn, int warehouseCount) {
    // TPC-C §3.3.2.5: every NEW-ORDER row pairs with an ORDER that has
    // O_CARRIER_ID = NULL, and every ORDER with O_CARRIER_ID = NULL has a
    // matching NEW-ORDER row. Carrier 0 is not a valid undelivered sentinel.
    const int RANGE_SIZE = 50;
    for (int startWh = 1; startWh <= warehouseCount; startWh += RANGE_SIZE) {
        int endWh = std::min(startWh + RANGE_SIZE - 1, warehouseCount);
        std::string sql = fmt::format(
            "SELECT * FROM ("
            "  SELECT no.NO_W_ID, no.NO_D_ID, no.NO_O_ID "
            "  FROM {} AS no "
            "  LEFT JOIN {} AS o "
            "    ON no.NO_W_ID = o.O_W_ID AND no.NO_D_ID = o.O_D_ID AND no.NO_O_ID = o.O_ID "
            "  WHERE no.NO_W_ID >= {} AND no.NO_W_ID <= {} "
            "    AND (o.O_W_ID IS NULL OR o.O_CARRIER_ID IS NOT NULL) "
            "  UNION ALL "
            "  SELECT o2.O_W_ID, o2.O_D_ID, o2.O_ID "
            "  FROM {} AS o2 "
            "  LEFT JOIN {} AS no2 "
            "    ON o2.O_W_ID = no2.NO_W_ID AND o2.O_D_ID = no2.NO_D_ID AND o2.O_ID = no2.NO_O_ID "
            "  WHERE o2.O_W_ID >= {} AND o2.O_W_ID <= {} "
            "    AND o2.O_CARRIER_ID IS NULL AND no2.NO_W_ID IS NULL"
            ") sub LIMIT 1",
            TABLE_NEW_ORDER, TABLE_OORDER, startWh, endWh,
            TABLE_OORDER, TABLE_NEW_ORDER, startWh, endWh);
        CheckNoRows(txn, sql, fmt::format("3.3.2.5 w_id [{},{}]", startWh, endWh));
    }
}

void ConsistencyCheck3326(pqxx::nontransaction& txn, int warehouseCount) {
    // For each order: O_OL_CNT must equal count of order_lines, and every order_line must have a parent order
    const int RANGE_SIZE = 50;
    for (int startWh = 1; startWh <= warehouseCount; startWh += RANGE_SIZE) {
        int endWh = std::min(startWh + RANGE_SIZE - 1, warehouseCount);
        std::string sql = fmt::format(
            "SELECT * FROM ("
            "  SELECT o.O_W_ID, o.O_D_ID, o.O_ID "
            "  FROM {} AS o "
            "  LEFT JOIN (SELECT OL_W_ID, OL_D_ID, OL_O_ID, COUNT(*) AS cnt "
            "             FROM {} WHERE OL_W_ID >= {} AND OL_W_ID <= {} "
            "             GROUP BY OL_W_ID, OL_D_ID, OL_O_ID) AS l "
            "    ON o.O_W_ID = l.OL_W_ID AND o.O_D_ID = l.OL_D_ID AND o.O_ID = l.OL_O_ID "
            "  WHERE o.O_W_ID >= {} AND o.O_W_ID <= {} AND o.O_OL_CNT != COALESCE(l.cnt, 0) "
            "  UNION ALL "
            "  SELECT l2.OL_W_ID, l2.OL_D_ID, l2.OL_O_ID "
            "  FROM (SELECT DISTINCT OL_W_ID, OL_D_ID, OL_O_ID FROM {} "
            "        WHERE OL_W_ID >= {} AND OL_W_ID <= {}) AS l2 "
            "  LEFT JOIN {} AS o2 "
            "    ON l2.OL_W_ID = o2.O_W_ID AND l2.OL_D_ID = o2.O_D_ID AND l2.OL_O_ID = o2.O_ID "
            "  WHERE o2.O_W_ID IS NULL"
            ") sub LIMIT 1",
            TABLE_OORDER, TABLE_ORDER_LINE, startWh, endWh, startWh, endWh,
            TABLE_ORDER_LINE, startWh, endWh, TABLE_OORDER);
        CheckNoRows(txn, sql, fmt::format("3.3.2.6 w_id [{},{}]", startWh, endWh));
    }
}

void ConsistencyCheck3327(pqxx::nontransaction& txn, int warehouseCount) {
    // TPC-C §3.3.2.7: O_CARRIER_ID IS NOT NULL <=> all OL_DELIVERY_D set;
    // O_CARRIER_ID IS NULL <=> all OL_DELIVERY_D null. Mixed delivery dates fail.
    const int RANGE_SIZE = 10;
    for (int startWh = 1; startWh <= warehouseCount; startWh += RANGE_SIZE) {
        int endWh = std::min(startWh + RANGE_SIZE - 1, warehouseCount);
        std::string sql = fmt::format(
            "SELECT l.OL_W_ID, l.OL_D_ID, l.OL_O_ID "
            "FROM ("
            "  SELECT OL_W_ID, OL_D_ID, OL_O_ID, "
            "    BOOL_OR(OL_DELIVERY_D IS NULL) AS some_null, "
            "    BOOL_OR(OL_DELIVERY_D IS NOT NULL) AS some_delivered "
            "  FROM {} WHERE OL_W_ID >= {} AND OL_W_ID <= {} "
            "  GROUP BY OL_W_ID, OL_D_ID, OL_O_ID"
            ") AS l "
            "JOIN {} AS o ON l.OL_W_ID = o.O_W_ID AND l.OL_D_ID = o.O_D_ID AND l.OL_O_ID = o.O_ID "
            "WHERE (l.some_null AND l.some_delivered) "
            "   OR (o.O_CARRIER_ID IS NULL AND l.some_delivered) "
            "   OR (o.O_CARRIER_ID IS NOT NULL AND l.some_null) "
            "LIMIT 1",
            TABLE_ORDER_LINE, startWh, endWh, TABLE_OORDER);
        CheckNoRows(txn, sql, fmt::format("3.3.2.7 w_id [{},{}]", startWh, endWh));
    }
}

void ConsistencyCheck3328(pqxx::nontransaction& txn) {
    // W_YTD = sum(H_AMOUNT) grouped by warehouse
    std::string sql = fmt::format(
        "SELECT w.W_ID, w.W_YTD, h.sum_h "
        "FROM {} AS w "
        "LEFT JOIN (SELECT H_W_ID, SUM(H_AMOUNT) AS sum_h FROM {} GROUP BY H_W_ID) AS h "
        "  ON w.W_ID = h.H_W_ID "
        "WHERE w.W_YTD IS DISTINCT FROM COALESCE(h.sum_h, 0) LIMIT 1",
        TABLE_WAREHOUSE, TABLE_HISTORY);
    CheckNoRows(txn, sql);
}

void ConsistencyCheck3329(pqxx::nontransaction& txn) {
    // D_YTD = sum(H_AMOUNT) grouped by warehouse+district
    std::string sql = fmt::format(
        "SELECT d.D_W_ID, d.D_ID, d.D_YTD, h.sum_h "
        "FROM {} AS d "
        "LEFT JOIN (SELECT H_W_ID, H_D_ID, SUM(H_AMOUNT) AS sum_h FROM {} GROUP BY H_W_ID, H_D_ID) AS h "
        "  ON d.D_W_ID = h.H_W_ID AND d.D_ID = h.H_D_ID "
        "WHERE d.D_YTD IS DISTINCT FROM COALESCE(h.sum_h, 0) LIMIT 1",
        TABLE_DISTRICT, TABLE_HISTORY);
    CheckNoRows(txn, sql);
}

void ConsistencyCheck33210(pqxx::nontransaction& txn, int warehouseCount) {
    // C_BALANCE = sum(delivered OL_AMOUNTs) - sum(H_AMOUNT) for each customer
    const int RANGE_SIZE = 10;
    for (int startWh = 1; startWh <= warehouseCount; startWh += RANGE_SIZE) {
        int endWh = std::min(startWh + RANGE_SIZE - 1, warehouseCount);
        std::string sql = fmt::format(
            "SELECT c.C_W_ID, c.C_D_ID, c.C_ID, c.C_BALANCE "
            "FROM {} AS c "
            "LEFT JOIN ("
            "  SELECT o.O_W_ID AS W_ID, o.O_D_ID AS D_ID, o.O_C_ID AS C_ID, SUM(ol.OL_AMOUNT) AS ol_sum "
            "  FROM {} AS o "
            "  JOIN {} AS ol ON ol.OL_W_ID = o.O_W_ID AND ol.OL_D_ID = o.O_D_ID AND ol.OL_O_ID = o.O_ID "
            "  WHERE ol.OL_DELIVERY_D IS NOT NULL AND o.O_W_ID >= {} AND o.O_W_ID <= {} "
            "  GROUP BY o.O_W_ID, o.O_D_ID, o.O_C_ID"
            ") AS ols ON c.C_W_ID = ols.W_ID AND c.C_D_ID = ols.D_ID AND c.C_ID = ols.C_ID "
            "LEFT JOIN ("
            "  SELECT H_C_W_ID, H_C_D_ID, H_C_ID, SUM(H_AMOUNT) AS h_sum "
            "  FROM {} WHERE H_C_W_ID >= {} AND H_C_W_ID <= {} "
            "  GROUP BY H_C_W_ID, H_C_D_ID, H_C_ID"
            ") AS hs ON c.C_W_ID = hs.H_C_W_ID AND c.C_D_ID = hs.H_C_D_ID AND c.C_ID = hs.H_C_ID "
            "WHERE c.C_W_ID >= {} AND c.C_W_ID <= {} "
            "  AND c.C_BALANCE IS DISTINCT FROM (COALESCE(ols.ol_sum, 0) - COALESCE(hs.h_sum, 0)) "
            "LIMIT 1",
            TABLE_CUSTOMER,
            TABLE_OORDER, TABLE_ORDER_LINE, startWh, endWh,
            TABLE_HISTORY, startWh, endWh,
            startWh, endWh);
        CheckNoRows(txn, sql, fmt::format("3.3.2.10 w_id [{},{}]", startWh, endWh));
    }
}

void ConsistencyCheck33211(pqxx::nontransaction& txn, int warehouseCount) {
    const int RANGE_SIZE = 50;
    for (int startWh = 1; startWh <= warehouseCount; startWh += RANGE_SIZE) {
        int endWh = std::min(startWh + RANGE_SIZE - 1, warehouseCount);
        std::string sql = fmt::format(
            "SELECT COALESCE(o.O_W_ID, n.NO_W_ID) AS w_id, "
            "       COALESCE(o.O_D_ID, n.NO_D_ID) AS d_id, "
            "       (COALESCE(o.order_cnt, 0) - COALESCE(n.new_order_cnt, 0)) AS delta "
            "FROM (SELECT O_W_ID, O_D_ID, COUNT(*) AS order_cnt FROM {} "
            "      WHERE O_W_ID >= {} AND O_W_ID <= {} GROUP BY O_W_ID, O_D_ID) AS o "
            "FULL JOIN (SELECT NO_W_ID, NO_D_ID, COUNT(*) AS new_order_cnt FROM {} "
            "      WHERE NO_W_ID >= {} AND NO_W_ID <= {} GROUP BY NO_W_ID, NO_D_ID) AS n "
            "  ON o.O_W_ID = n.NO_W_ID AND o.O_D_ID = n.NO_D_ID "
            "WHERE (COALESCE(o.order_cnt, 0) - COALESCE(n.new_order_cnt, 0)) != {} LIMIT 1",
            TABLE_OORDER, startWh, endWh,
            TABLE_NEW_ORDER, startWh, endWh,
            FIRST_UNPROCESSED_O_ID - 1);
        CheckNoRows(txn, sql, fmt::format("3.3.2.11 w_id [{},{}]", startWh, endWh));
    }
}

void ConsistencyCheck33212(pqxx::nontransaction& txn, int warehouseCount) {
    // C_BALANCE + C_YTD_PAYMENT = sum(delivered OL_AMOUNTs) for each customer
    // LEFT JOIN so never-delivered customers are still checked (expect 0).
    const int RANGE_SIZE = 10;
    for (int startWh = 1; startWh <= warehouseCount; startWh += RANGE_SIZE) {
        int endWh = std::min(startWh + RANGE_SIZE - 1, warehouseCount);
        std::string sql = fmt::format(
            "SELECT c.C_W_ID, c.C_D_ID, c.C_ID "
            "FROM {} AS c "
            "LEFT JOIN ("
            "  SELECT o.O_W_ID AS W_ID, o.O_D_ID AS D_ID, o.O_C_ID AS C_ID, SUM(ol.OL_AMOUNT) AS ol_sum "
            "  FROM {} AS o "
            "  JOIN {} AS ol ON ol.OL_W_ID = o.O_W_ID AND ol.OL_D_ID = o.O_D_ID AND ol.OL_O_ID = o.O_ID "
            "  WHERE ol.OL_DELIVERY_D IS NOT NULL AND o.O_W_ID >= {} AND o.O_W_ID <= {} "
            "  GROUP BY o.O_W_ID, o.O_D_ID, o.O_C_ID"
            ") AS l ON c.C_W_ID = l.W_ID AND c.C_D_ID = l.D_ID AND c.C_ID = l.C_ID "
            "WHERE c.C_W_ID >= {} AND c.C_W_ID <= {} "
            "  AND (c.C_BALANCE + c.C_YTD_PAYMENT) IS DISTINCT FROM COALESCE(l.ol_sum, 0) "
            "LIMIT 1",
            TABLE_CUSTOMER,
            TABLE_OORDER, TABLE_ORDER_LINE, startWh, endWh,
            startWh, endWh);
        CheckNoRows(txn, sql, fmt::format("3.3.2.12 w_id [{},{}]", startWh, endWh));
    }
}

//-----------------------------------------------------------------------------
// Post-import checks: stricter invariants that hold only on freshly loaded data
//-----------------------------------------------------------------------------

void PostImportCheckNextOrderId(pqxx::nontransaction& txn) {
    std::string sql = fmt::format(
        "SELECT D_W_ID, D_ID, D_NEXT_O_ID FROM {} "
        "WHERE D_NEXT_O_ID != {} LIMIT 1",
        TABLE_DISTRICT, CUSTOMERS_PER_DISTRICT + 1);
    CheckNoRows(txn, sql, fmt::format(
        "D_NEXT_O_ID must be {} for all districts after import", CUSTOMERS_PER_DISTRICT + 1));
}

void PostImportCheckWarehouseYtd(pqxx::nontransaction& txn) {
    const std::string expectedYtd = WAREHOUSE_INITIAL_YTD.ToString();
    std::string sql = fmt::format(
        "SELECT W_ID, W_YTD FROM {} WHERE W_YTD <> {}::numeric LIMIT 1",
        TABLE_WAREHOUSE, expectedYtd);
    CheckNoRows(txn, sql, fmt::format("W_YTD must be {} after import", expectedYtd));
}

void PostImportCheckDistrictYtd(pqxx::nontransaction& txn) {
    const std::string expectedYtd = DISTRICT_INITIAL_YTD.ToString();
    std::string sql = fmt::format(
        "SELECT D_W_ID, D_ID, D_YTD FROM {} WHERE D_YTD <> {}::numeric LIMIT 1",
        TABLE_DISTRICT, expectedYtd);
    CheckNoRows(txn, sql, fmt::format("D_YTD must be {} after import", expectedYtd));
}

void PostImportCheckNoCarriers(pqxx::nontransaction& txn) {
    std::string sql = fmt::format(
        "SELECT O_W_ID, O_D_ID, O_ID, O_CARRIER_ID FROM {} "
        "WHERE O_ID >= {} AND O_CARRIER_ID IS NOT NULL LIMIT 1",
        TABLE_OORDER, FIRST_UNPROCESSED_O_ID);
    CheckNoRows(txn, sql, "Unprocessed orders must have NULL O_CARRIER_ID after import");
}

void PostImportCheckCarrierRange(pqxx::nontransaction& txn) {
    // TPC-C §4.3.3.1: initially delivered orders have O_CARRIER_ID randomly selected
    // unique within [1 .. 10].
    std::string sql = fmt::format(
        "SELECT O_W_ID, O_D_ID, O_ID, O_CARRIER_ID FROM {} "
        "WHERE O_ID < {} AND (O_CARRIER_ID IS NULL OR O_CARRIER_ID < 1 OR O_CARRIER_ID > 10) "
        "LIMIT 1",
        TABLE_OORDER, FIRST_UNPROCESSED_O_ID);
    CheckNoRows(txn, sql, "Delivered orders must have O_CARRIER_ID in [1..10] after import");
}

void PostImportCheckNoDeliveryDates(pqxx::nontransaction& txn) {
    std::string sql = fmt::format(
        "SELECT ol.OL_W_ID, ol.OL_D_ID, ol.OL_O_ID FROM {} AS ol "
        "WHERE ol.OL_O_ID >= {} AND ol.OL_DELIVERY_D IS NOT NULL LIMIT 1",
        TABLE_ORDER_LINE, FIRST_UNPROCESSED_O_ID);
    CheckNoRows(txn, sql, "Unprocessed order lines must have NULL OL_DELIVERY_D after import");
}

void PostImportCheckDeliveryEqualsEntry(pqxx::nontransaction& txn) {
    // TPC-C §4.3.3.1: for initially delivered orders, OL_DELIVERY_D = O_ENTRY_D.
    std::string sql = fmt::format(
        "SELECT ol.OL_W_ID, ol.OL_D_ID, ol.OL_O_ID, ol.OL_NUMBER "
        "FROM {} AS ol "
        "JOIN {} AS o ON o.O_W_ID = ol.OL_W_ID AND o.O_D_ID = ol.OL_D_ID AND o.O_ID = ol.OL_O_ID "
        "WHERE ol.OL_O_ID < {} AND "
        "(ol.OL_DELIVERY_D IS NULL OR ol.OL_DELIVERY_D IS DISTINCT FROM o.O_ENTRY_D) "
        "LIMIT 1",
        TABLE_ORDER_LINE, TABLE_OORDER, FIRST_UNPROCESSED_O_ID);
    CheckNoRows(txn, sql, "Delivered order lines must have OL_DELIVERY_D = O_ENTRY_D after import");
}

void PostImportCheckDeliveredAmountZero(pqxx::nontransaction& txn) {
    // TPC-C §4.3.3.1: for initially delivered order lines, OL_AMOUNT = 0.00.
    std::string sql = fmt::format(
        "SELECT OL_W_ID, OL_D_ID, OL_O_ID, OL_NUMBER, OL_AMOUNT FROM {} "
        "WHERE OL_O_ID < {} AND OL_AMOUNT IS DISTINCT FROM 0.00 "
        "LIMIT 1",
        TABLE_ORDER_LINE, FIRST_UNPROCESSED_O_ID);
    CheckNoRows(txn, sql, "Delivered order lines must have OL_AMOUNT = 0.00 after import");
}

void RecordResult(TCheckReport& report, const std::string& id, ECheckStatus status,
                  const std::string& detail, bool print) {
    TCheckResult r;
    r.Id = id;
    if (const auto* entry = FindCheckCatalogEntry(id)) {
        r.Title = std::string(entry->Title);
    } else {
        r.Title = id;
    }
    r.Status = status;
    r.Detail = detail;
    switch (status) {
        case ECheckStatus::Passed: ++report.PassedCount; break;
        case ECheckStatus::Failed: ++report.FailedCount; break;
        case ECheckStatus::Skipped: ++report.SkippedCount; break;
        case ECheckStatus::Error: ++report.ErrorCount; break;
    }
    if (print) {
        const char* tag = "[OK]";
        if (status == ECheckStatus::Failed || status == ECheckStatus::Error) {
            tag = "[Failed]";
        } else if (status == ECheckStatus::Skipped) {
            tag = "[Skipped]";
        }
        std::cout << "Checking " << r.Title << " " << tag;
        if (!detail.empty() && status != ECheckStatus::Passed) {
            std::cout << ": " << detail;
        }
        std::cout << std::endl;
    }
    report.Results.push_back(std::move(r));
}

void RunOne(TCheckReport& report, const std::string& id, bool print, auto&& fn) {
    try {
        fn();
        RecordResult(report, id, ECheckStatus::Passed, {}, print);
    } catch (const std::exception& ex) {
        RecordResult(report, id, ECheckStatus::Failed, ex.what(), print);
    }
}

} // anonymous

TCheckReport RunPgChecks(const std::string& connectionString, const TCheckRequest& request) {
    TCheckReport report;
    report.RunId = request.RunId;
    report.Instance = request.Instance;
    report.Phase = request.Phase == ECheckPhase::AfterImport ? "after-import" : "after-run";
    report.WarehouseCount = request.WarehouseCount;

    const bool print = true;
    const bool afterImport = request.Phase == ECheckPhase::AfterImport;

    if (request.WarehouseCount <= 0) {
        RecordResult(report, "cardinality.warehouse", ECheckStatus::Error,
                     "Zero warehouses specified", print);
        return report;
    }

    try {
        pqxx::connection conn(connectionString);

        if (!request.Path.empty()) {
            pqxx::nontransaction ntx(conn);
            ntx.exec(fmt::format("SET search_path TO {}", conn.quote_name(request.Path)));
        }

        pqxx::nontransaction txn(conn);

        RunOne(report, "cardinality.warehouse", print,
               [&] { BaseCheckWarehouseTable(txn, request.WarehouseCount); });
        RunOne(report, "cardinality.district", print,
               [&] { BaseCheckDistrictTable(txn, request.WarehouseCount); });
        RunOne(report, "cardinality.customer", print,
               [&] { BaseCheckCustomerTable(txn, request.WarehouseCount); });
        RunOne(report, "cardinality.item", print,
               [&] { BaseCheckItemTable(txn); });
        RunOne(report, "cardinality.stock", print,
               [&] { BaseCheckStockTable(txn, request.WarehouseCount); });

        if (afterImport) {
            RunOne(report, "cardinality.oorder", print,
                   [&] { BaseCheckOorderTable(txn, request.WarehouseCount); });
            RunOne(report, "cardinality.new_order", print,
                   [&] { BaseCheckNewOrderTable(txn, request.WarehouseCount); });
            RunOne(report, "cardinality.order_line", print,
                   [&] { BaseCheckOrderLineTable(txn, request.WarehouseCount); });
            RunOne(report, "cardinality.history", print,
                   [&] { BaseCheckHistoryTable(txn, request.WarehouseCount); });
        }

        // Abort consistency suite if base cardinality already failed.
        bool baseFailed = false;
        for (const auto& r : report.Results) {
            if (r.Id.rfind("cardinality.", 0) == 0 &&
                (r.Status == ECheckStatus::Failed || r.Status == ECheckStatus::Error)) {
                baseFailed = true;
                break;
            }
        }
        if (baseFailed) {
            std::cout << "Base checks failed, aborting consistency checks!" << std::endl;
            for (const auto& entry : CheckCatalog()) {
                if (std::string(entry.Id).rfind("cardinality.", 0) == 0) {
                    continue;
                }
                if (!CheckAppliesToPhase(entry.Phase, request.Phase)) {
                    continue;
                }
                RecordResult(report, std::string(entry.Id), ECheckStatus::Skipped,
                             "skipped: base cardinality failed", print);
            }
            return report;
        }

        RunOne(report, "consistency.3.3.2.1", print, [&] { ConsistencyCheck3321(txn); });
        RunOne(report, "consistency.3.3.2.2", print, [&] { ConsistencyCheck3322(txn); });
        RunOne(report, "consistency.3.3.2.3", print, [&] { ConsistencyCheck3323(txn); });
        RunOne(report, "consistency.3.3.2.4", print,
               [&] { ConsistencyCheck3324(txn, request.WarehouseCount); });
        RunOne(report, "consistency.3.3.2.5", print,
               [&] { ConsistencyCheck3325(txn, request.WarehouseCount); });
        RunOne(report, "consistency.3.3.2.6", print,
               [&] { ConsistencyCheck3326(txn, request.WarehouseCount); });
        RunOne(report, "consistency.3.3.2.7", print,
               [&] { ConsistencyCheck3327(txn, request.WarehouseCount); });
        RunOne(report, "consistency.3.3.2.8", print, [&] { ConsistencyCheck3328(txn); });
        RunOne(report, "consistency.3.3.2.9", print, [&] { ConsistencyCheck3329(txn); });
        RunOne(report, "consistency.3.3.2.10", print,
               [&] { ConsistencyCheck33210(txn, request.WarehouseCount); });
        RunOne(report, "consistency.3.3.2.12", print,
               [&] { ConsistencyCheck33212(txn, request.WarehouseCount); });

        if (afterImport) {
            RunOne(report, "consistency.3.3.2.11", print,
                   [&] { ConsistencyCheck33211(txn, request.WarehouseCount); });
            RunOne(report, "post_import.d_next_o_id", print,
                   [&] { PostImportCheckNextOrderId(txn); });
            RunOne(report, "post_import.w_ytd", print,
                   [&] { PostImportCheckWarehouseYtd(txn); });
            RunOne(report, "post_import.d_ytd", print,
                   [&] { PostImportCheckDistrictYtd(txn); });
            RunOne(report, "post_import.o_carrier_id", print,
                   [&] { PostImportCheckNoCarriers(txn); });
            RunOne(report, "post_import.o_carrier_id_range", print,
                   [&] { PostImportCheckCarrierRange(txn); });
            RunOne(report, "post_import.ol_delivery_d", print,
                   [&] { PostImportCheckNoDeliveryDates(txn); });
            RunOne(report, "post_import.ol_delivery_eq_entry", print,
                   [&] { PostImportCheckDeliveryEqualsEntry(txn); });
            RunOne(report, "post_import.ol_amount_delivered", print,
                   [&] { PostImportCheckDeliveredAmountZero(txn); });
        }
    } catch (const std::exception& ex) {
        RecordResult(report, "connection", ECheckStatus::Error, ex.what(), print);
    }

    if (report.Ok()) {
        std::cout << "Everything is good!" << std::endl;
    }
    return report;
}

void CheckSync(const std::string& connectionString, int warehouseCount, bool afterImport,
               const std::string& path) {
    TCheckRequest req;
    req.WarehouseCount = warehouseCount;
    req.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterRun;
    req.Path = path;
    auto report = RunPgChecks(connectionString, req);
    if (!report.Ok()) {
        throw std::runtime_error(fmt::format("{} checks failed", report.FailedCount + report.ErrorCount));
    }
}

TPgCheckAdapter::TPgCheckAdapter(std::string connectionString)
    : ConnectionString_(std::move(connectionString))
{
}

TCheckReport TPgCheckAdapter::Run(const TCheckRequest& request) {
    return RunPgChecks(ConnectionString_, request);
}

int RunCheckFromRunConfig(const std::string& runConfigPath, const std::string& instance,
                          bool afterImport, bool afterRun) {
    if (afterImport == afterRun) {
        throw std::runtime_error("check requires exactly one of --after-import or --after-run");
    }

    const auto doc = LoadRunConfigDocument(runConfigPath);
    const std::string connection = BuildPgConnectionString(doc);
    CheckDbForRun(connection, doc.ScaleWarehouses, doc.Path);

    TCheckRequest req;
    req.WarehouseCount = doc.ScaleWarehouses;
    req.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterRun;
    req.Path = doc.Path;
    req.RunId = doc.RunId;
    req.Instance = instance;

    TPgCheckAdapter adapter(connection);
    const auto report = adapter.Run(req);

    const std::string checksDir = doc.RunDir + "/checks";
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

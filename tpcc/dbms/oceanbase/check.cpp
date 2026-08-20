#include "check.h"

#include "ob_connection.h"
#include "path_checker.h"
#include "run_config.h"

#include <artifacts.h>
#include <catalog.h>
#include <constants.h>
#include <log.h>
#include <report.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace NTpcc {

namespace {

QueryResult QueryOne(TObConnection& conn, const std::string& sql) {
    auto result = conn.QuerySimple(sql);
    if (!result.TryNextRow()) {
        throw std::runtime_error("Expected one row, got none: " + sql);
    }
    return result;
}

void CheckNoRows(TObConnection& conn, const std::string& sql, const std::string& description = {}) {
    auto result = conn.QuerySimple(sql);
    if (result.TryNextRow()) {
        throw std::runtime_error(description.empty() ? "Unexpected rows returned" : description);
    }
}

void BaseCheckWarehouseTable(TObConnection& conn, int expectedWhNumber) {
    auto r = QueryOne(conn, fmt::format(
        "SELECT COUNT(*) AS cnt, MAX(w_id) AS max_id, MIN(w_id) AS min_id FROM {}", TABLE_WAREHOUSE));
    if (r.GetInt64("cnt") != expectedWhNumber ||
        r.GetInt32("min_id") != 1 ||
        r.GetInt32("max_id") != expectedWhNumber)
    {
        throw std::runtime_error(fmt::format(
            "Inconsistent warehouse: count={}, min={}, max={}, expected={}",
            r.GetInt64("cnt"), r.GetInt32("min_id"), r.GetInt32("max_id"), expectedWhNumber));
    }
}

void BaseCheckDistrictTable(TObConnection& conn, int expectedWhNumber) {
    auto r = QueryOne(conn, fmt::format(
        "SELECT COUNT(*) AS cnt, MAX(d_w_id) AS max_w, MIN(d_w_id) AS min_w, "
        "MAX(d_id) AS max_d, MIN(d_id) AS min_d FROM {}", TABLE_DISTRICT));
    const int expectedCount = expectedWhNumber * DISTRICT_COUNT;
    if (r.GetInt64("cnt") != expectedCount ||
        r.GetInt32("min_w") != 1 || r.GetInt32("max_w") != expectedWhNumber ||
        r.GetInt32("min_d") != DISTRICT_LOW_ID || r.GetInt32("max_d") != DISTRICT_HIGH_ID)
    {
        throw std::runtime_error("District cardinality/range mismatch");
    }
}

void BaseCheckCustomerTable(TObConnection& conn, int startWh, int endWh) {
    auto r = QueryOne(conn, fmt::format(
        "SELECT COUNT(*) AS cnt, MAX(c_w_id) AS max_w, MIN(c_w_id) AS min_w, "
        "MAX(c_d_id) AS max_d, MIN(c_d_id) AS min_d, MAX(c_id) AS max_c, MIN(c_id) AS min_c "
        "FROM {} WHERE c_w_id >= {} AND c_w_id <= {}",
        TABLE_CUSTOMER, startWh, endWh));
    const int rangeWh = endWh - startWh + 1;
    const int expectedCount = rangeWh * CUSTOMERS_PER_DISTRICT * DISTRICT_COUNT;
    if (r.GetInt64("cnt") != expectedCount ||
        r.GetInt32("min_w") != startWh || r.GetInt32("max_w") != endWh ||
        r.GetInt32("min_d") != DISTRICT_LOW_ID || r.GetInt32("max_d") != DISTRICT_HIGH_ID ||
        r.GetInt32("min_c") != 1 || r.GetInt32("max_c") != CUSTOMERS_PER_DISTRICT)
    {
        throw std::runtime_error(fmt::format(
            "Customer cardinality/range mismatch (w_id [{},{}])", startWh, endWh));
    }
}

void BaseCheckItemTable(TObConnection& conn) {
    auto r = QueryOne(conn, fmt::format(
        "SELECT COUNT(*) AS cnt, MAX(i_id) AS max_id, MIN(i_id) AS min_id FROM {}", TABLE_ITEM));
    if (r.GetInt64("cnt") != ITEM_COUNT ||
        r.GetInt32("min_id") != 1 ||
        r.GetInt32("max_id") != ITEM_COUNT)
    {
        throw std::runtime_error("Item cardinality/range mismatch");
    }
}

void BaseCheckStockTable(TObConnection& conn, int startWh, int endWh) {
    auto r = QueryOne(conn, fmt::format(
        "SELECT COUNT(*) AS cnt, COUNT(DISTINCT s_w_id) AS wh_cnt, "
        "MAX(s_w_id) AS max_w, MIN(s_w_id) AS min_w, MAX(s_i_id) AS max_i, MIN(s_i_id) AS min_i "
        "FROM {} WHERE s_w_id >= {} AND s_w_id <= {}",
        TABLE_STOCK, startWh, endWh));
    const int rangeWh = endWh - startWh + 1;
    const int expectedCount = rangeWh * ITEM_COUNT;
    if (r.GetInt64("cnt") != expectedCount ||
        r.GetInt32("wh_cnt") != rangeWh ||
        r.GetInt32("min_w") != startWh || r.GetInt32("max_w") != endWh ||
        r.GetInt32("min_i") != 1 || r.GetInt32("max_i") != ITEM_COUNT)
    {
        throw std::runtime_error(fmt::format(
            "Stock cardinality/range mismatch (w_id [{},{}])", startWh, endWh));
    }
}

void BaseCheckOorderTable(TObConnection& conn, int startWh, int endWh) {
    auto r = QueryOne(conn, fmt::format(
        "SELECT COUNT(*) AS cnt, MAX(o_w_id) AS max_w, MIN(o_w_id) AS min_w, "
        "MAX(o_d_id) AS max_d, MIN(o_d_id) AS min_d, MAX(o_id) AS max_o, MIN(o_id) AS min_o "
        "FROM {} WHERE o_w_id >= {} AND o_w_id <= {}",
        TABLE_OORDER, startWh, endWh));
    const int rangeWh = endWh - startWh + 1;
    const int expectedCount = rangeWh * CUSTOMERS_PER_DISTRICT * DISTRICT_COUNT;
    if (r.GetInt64("cnt") != expectedCount ||
        r.GetInt32("min_w") != startWh || r.GetInt32("max_w") != endWh ||
        r.GetInt32("min_d") != DISTRICT_LOW_ID || r.GetInt32("max_d") != DISTRICT_HIGH_ID ||
        r.GetInt32("min_o") != 1 || r.GetInt32("max_o") != CUSTOMERS_PER_DISTRICT)
    {
        throw std::runtime_error(fmt::format(
            "Order cardinality/range mismatch (w_id [{},{}])", startWh, endWh));
    }
}

void BaseCheckNewOrderTable(TObConnection& conn, int startWh, int endWh) {
    auto r = QueryOne(conn, fmt::format(
        "SELECT COUNT(*) AS cnt, MAX(no_w_id) AS max_w, MIN(no_w_id) AS min_w, "
        "MAX(no_d_id) AS max_d, MIN(no_d_id) AS min_d, MAX(no_o_id) AS max_o, MIN(no_o_id) AS min_o "
        "FROM {} WHERE no_w_id >= {} AND no_w_id <= {}",
        TABLE_NEW_ORDER, startWh, endWh));
    const int rangeWh = endWh - startWh + 1;
    const int expectedCount = rangeWh *
        (CUSTOMERS_PER_DISTRICT - FIRST_UNPROCESSED_O_ID + 1) * DISTRICT_COUNT;
    if (r.GetInt64("cnt") != expectedCount ||
        r.GetInt32("min_w") != startWh || r.GetInt32("max_w") != endWh ||
        r.GetInt32("min_d") != DISTRICT_LOW_ID || r.GetInt32("max_d") != DISTRICT_HIGH_ID ||
        r.GetInt32("min_o") < FIRST_UNPROCESSED_O_ID ||
        r.GetInt32("max_o") != CUSTOMERS_PER_DISTRICT)
    {
        throw std::runtime_error(fmt::format(
            "New-order cardinality/range mismatch (w_id [{},{}])", startWh, endWh));
    }
}

void BaseCheckOrderLineTable(TObConnection& conn, int startWh, int endWh) {
    auto r = QueryOne(conn, fmt::format(
        "SELECT MIN(order_count) AS min_orders, MAX(order_count) AS max_orders, COUNT(*) AS district_count "
        "FROM (SELECT ol_w_id, ol_d_id, COUNT(DISTINCT ol_o_id) AS order_count "
        "FROM {} WHERE ol_w_id >= {} AND ol_w_id <= {} GROUP BY ol_w_id, ol_d_id) sub",
        TABLE_ORDER_LINE, startWh, endWh));
    const int rangeWh = endWh - startWh + 1;
    const int expectedDistrictCount = rangeWh * DISTRICT_COUNT;
    if (r.GetInt64("district_count") != expectedDistrictCount ||
        r.GetInt64("min_orders") != CUSTOMERS_PER_DISTRICT ||
        r.GetInt64("max_orders") != CUSTOMERS_PER_DISTRICT)
    {
        throw std::runtime_error(fmt::format(
            "Order-line cardinality/range mismatch (w_id [{},{}])", startWh, endWh));
    }
}

void BaseCheckHistoryTable(TObConnection& conn, int startWh, int endWh) {
    auto r = QueryOne(conn, fmt::format(
        "SELECT COUNT(*) AS cnt, MAX(h_c_w_id) AS max_w, MIN(h_c_w_id) AS min_w FROM {} "
        "WHERE h_c_w_id >= {} AND h_c_w_id <= {}",
        TABLE_HISTORY, startWh, endWh));
    const int rangeWh = endWh - startWh + 1;
    const int expectedCount = rangeWh * CUSTOMERS_PER_DISTRICT * DISTRICT_COUNT;
    if (r.GetInt64("cnt") != expectedCount ||
        r.GetInt32("min_w") != startWh ||
        r.GetInt32("max_w") != endWh)
    {
        throw std::runtime_error(fmt::format(
            "History cardinality/range mismatch (w_id [{},{}])", startWh, endWh));
    }
}

void ConsistencyCheck3321(TObConnection& conn) {
    // Exact DECIMAL compare (null-safe <=>); epsilon is not allowed (PG reference / §5.12).
    CheckNoRows(conn, fmt::format(
        "SELECT w.w_id FROM {} AS w "
        "LEFT JOIN (SELECT d_w_id, SUM(d_ytd) AS sum_d_ytd FROM {} GROUP BY d_w_id) AS d "
        "ON w.w_id = d.d_w_id WHERE NOT (w.w_ytd <=> COALESCE(d.sum_d_ytd, 0)) LIMIT 1",
        TABLE_WAREHOUSE, TABLE_DISTRICT));
}

void ConsistencyCheck3322(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT d.d_w_id FROM {} AS d "
        "LEFT JOIN (SELECT o_w_id, o_d_id, MAX(o_id) AS max_o_id FROM {} "
        "           WHERE o_w_id >= {} AND o_w_id <= {} GROUP BY o_w_id, o_d_id) AS o "
        "ON d.d_w_id = o.o_w_id AND d.d_id = o.o_d_id "
        "LEFT JOIN (SELECT no_w_id, no_d_id, MAX(no_o_id) AS max_no_o_id FROM {} "
        "           WHERE no_w_id >= {} AND no_w_id <= {} GROUP BY no_w_id, no_d_id) AS n "
        "ON d.d_w_id = n.no_w_id AND d.d_id = n.no_d_id "
        "WHERE d.d_w_id >= {} AND d.d_w_id <= {} "
        "AND (o.max_o_id IS NULL OR (d.d_next_o_id - 1) != o.max_o_id "
        "OR (n.max_no_o_id IS NOT NULL AND n.max_no_o_id != o.max_o_id)) LIMIT 1",
        TABLE_DISTRICT, TABLE_OORDER, startWh, endWh, TABLE_NEW_ORDER, startWh, endWh,
        startWh, endWh),
        fmt::format("3.3.2.2 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck3323(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT no_w_id FROM {} WHERE no_w_id >= {} AND no_w_id <= {} "
        "GROUP BY no_w_id, no_d_id "
        "HAVING COUNT(*) - (MAX(no_o_id) - MIN(no_o_id) + 1) != 0 LIMIT 1",
        TABLE_NEW_ORDER, startWh, endWh),
        fmt::format("3.3.2.3 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck3324(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT * FROM ("
        "SELECT o.o_w_id, o.o_d_id FROM (SELECT o_w_id, o_d_id, SUM(o_ol_cnt) AS sum_ol_cnt "
        "FROM {0} WHERE o_w_id >= {1} AND o_w_id <= {2} GROUP BY o_w_id, o_d_id) AS o "
        "LEFT JOIN (SELECT ol_w_id, ol_d_id, COUNT(*) AS ol_count FROM {3} "
        "WHERE ol_w_id >= {1} AND ol_w_id <= {2} GROUP BY ol_w_id, ol_d_id) AS ol "
        "ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id "
        "WHERE COALESCE(o.sum_ol_cnt, -1) != COALESCE(ol.ol_count, -1) "
        "UNION ALL "
        "SELECT ol2.ol_w_id, ol2.ol_d_id FROM (SELECT ol_w_id, ol_d_id, COUNT(*) AS ol_count "
        "FROM {3} WHERE ol_w_id >= {1} AND ol_w_id <= {2} GROUP BY ol_w_id, ol_d_id) AS ol2 "
        "LEFT JOIN (SELECT o_w_id, o_d_id, SUM(o_ol_cnt) AS sum_ol_cnt FROM {0} "
        "WHERE o_w_id >= {1} AND o_w_id <= {2} GROUP BY o_w_id, o_d_id) AS o2 "
        "ON o2.o_w_id = ol2.ol_w_id AND o2.o_d_id = ol2.ol_d_id WHERE o2.o_w_id IS NULL"
        ") sub LIMIT 1", TABLE_OORDER, startWh, endWh, TABLE_ORDER_LINE),
        fmt::format("3.3.2.4 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck3325(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT * FROM ("
        "SELECT no.no_w_id, no.no_d_id, no.no_o_id FROM {0} AS no LEFT JOIN {1} AS o "
        "ON no.no_w_id = o.o_w_id AND no.no_d_id = o.o_d_id AND no.no_o_id = o.o_id "
        "WHERE no.no_w_id >= {2} AND no.no_w_id <= {3} AND (o.o_w_id IS NULL OR o.o_carrier_id IS NOT NULL) "
        "UNION ALL "
        "SELECT o2.o_w_id, o2.o_d_id, o2.o_id FROM {1} AS o2 LEFT JOIN {0} AS no2 "
        "ON o2.o_w_id = no2.no_w_id AND o2.o_d_id = no2.no_d_id AND o2.o_id = no2.no_o_id "
        "WHERE o2.o_w_id >= {2} AND o2.o_w_id <= {3} AND o2.o_carrier_id IS NULL AND no2.no_w_id IS NULL"
        ") sub LIMIT 1", TABLE_NEW_ORDER, TABLE_OORDER, startWh, endWh),
        fmt::format("3.3.2.5 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck3326(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT * FROM ("
        "SELECT o.o_w_id, o.o_d_id, o.o_id FROM {0} AS o "
        "LEFT JOIN (SELECT ol_w_id, ol_d_id, ol_o_id, COUNT(*) AS cnt FROM {1} "
        "WHERE ol_w_id >= {2} AND ol_w_id <= {3} GROUP BY ol_w_id, ol_d_id, ol_o_id) AS l "
        "ON o.o_w_id = l.ol_w_id AND o.o_d_id = l.ol_d_id AND o.o_id = l.ol_o_id "
        "WHERE o.o_w_id >= {2} AND o.o_w_id <= {3} AND o.o_ol_cnt != COALESCE(l.cnt, 0) "
        "UNION ALL "
        "SELECT l2.ol_w_id, l2.ol_d_id, l2.ol_o_id FROM "
        "(SELECT DISTINCT ol_w_id, ol_d_id, ol_o_id FROM {1} WHERE ol_w_id >= {2} AND ol_w_id <= {3}) AS l2 "
        "LEFT JOIN {0} AS o2 ON l2.ol_w_id = o2.o_w_id AND l2.ol_d_id = o2.o_d_id AND l2.ol_o_id = o2.o_id "
        "WHERE o2.o_w_id IS NULL"
        ") sub LIMIT 1", TABLE_OORDER, TABLE_ORDER_LINE, startWh, endWh),
        fmt::format("3.3.2.6 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck3327(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT l.ol_w_id FROM (SELECT ol_w_id, ol_d_id, ol_o_id, "
        "MIN(CASE WHEN ol_delivery_d IS NOT NULL THEN 1 ELSE 0 END) AS all_delivered, "
        "MAX(CASE WHEN ol_delivery_d IS NULL THEN 1 ELSE 0 END) AS some_null, "
        "MAX(CASE WHEN ol_delivery_d IS NOT NULL THEN 1 ELSE 0 END) AS some_delivered "
        "FROM {} WHERE ol_w_id >= {} AND ol_w_id <= {} GROUP BY ol_w_id, ol_d_id, ol_o_id) AS l "
        "JOIN {} AS o ON l.ol_w_id = o.o_w_id AND l.ol_d_id = o.o_d_id AND l.ol_o_id = o.o_id "
        "WHERE (l.some_null = 1 AND l.some_delivered = 1) "
        "OR (o.o_carrier_id IS NULL AND l.some_delivered = 1) "
        "OR (o.o_carrier_id IS NOT NULL AND l.some_null = 1) LIMIT 1",
        TABLE_ORDER_LINE, startWh, endWh, TABLE_OORDER),
        fmt::format("3.3.2.7 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck3328(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT w.w_id FROM {} AS w LEFT JOIN "
        "(SELECT h_w_id, SUM(h_amount) AS sum_h FROM {} "
        " WHERE h_w_id >= {} AND h_w_id <= {} GROUP BY h_w_id) AS h "
        "ON w.w_id = h.h_w_id "
        "WHERE w.w_id >= {} AND w.w_id <= {} "
        "AND NOT (w.w_ytd <=> COALESCE(h.sum_h, 0)) LIMIT 1",
        TABLE_WAREHOUSE, TABLE_HISTORY, startWh, endWh, startWh, endWh),
        fmt::format("3.3.2.8 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck3329(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT d.d_w_id FROM {} AS d LEFT JOIN "
        "(SELECT h_w_id, h_d_id, SUM(h_amount) AS sum_h FROM {} "
        " WHERE h_w_id >= {} AND h_w_id <= {} GROUP BY h_w_id, h_d_id) AS h "
        "ON d.d_w_id = h.h_w_id AND d.d_id = h.h_d_id "
        "WHERE d.d_w_id >= {} AND d.d_w_id <= {} "
        "AND NOT (d.d_ytd <=> COALESCE(h.sum_h, 0)) LIMIT 1",
        TABLE_DISTRICT, TABLE_HISTORY, startWh, endWh, startWh, endWh),
        fmt::format("3.3.2.9 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck33210(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT c.c_w_id FROM {0} AS c "
        "LEFT JOIN (SELECT o.o_w_id AS w_id, o.o_d_id AS d_id, o.o_c_id AS c_id, SUM(ol.ol_amount) AS ol_sum "
        "FROM {1} AS o JOIN {2} AS ol ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id "
        "WHERE ol.ol_delivery_d IS NOT NULL AND o.o_w_id >= {3} AND o.o_w_id <= {4} GROUP BY o.o_w_id, o.o_d_id, o.o_c_id) AS ols "
        "ON c.c_w_id = ols.w_id AND c.c_d_id = ols.d_id AND c.c_id = ols.c_id "
        "LEFT JOIN (SELECT h_c_w_id, h_c_d_id, h_c_id, SUM(h_amount) AS h_sum FROM {5} "
        "WHERE h_c_w_id >= {3} AND h_c_w_id <= {4} GROUP BY h_c_w_id, h_c_d_id, h_c_id) AS hs "
        "ON c.c_w_id = hs.h_c_w_id AND c.c_d_id = hs.h_c_d_id AND c.c_id = hs.h_c_id "
        "WHERE c.c_w_id >= {3} AND c.c_w_id <= {4} "
        "AND NOT (c.c_balance <=> (COALESCE(ols.ol_sum, 0) - COALESCE(hs.h_sum, 0))) LIMIT 1",
        TABLE_CUSTOMER, TABLE_OORDER, TABLE_ORDER_LINE, startWh, endWh, TABLE_HISTORY),
        fmt::format("3.3.2.10 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck33211(TObConnection& conn, int startWh, int endWh) {
    // FULL OUTER JOIN via LEFT+UNION ALL (MySQL mode); matches PG 3.3.2.11.
    CheckNoRows(conn, fmt::format(
        "SELECT * FROM ("
        "SELECT o.o_w_id, o.o_d_id FROM (SELECT o_w_id, o_d_id, COUNT(*) AS order_cnt FROM {0} "
        "WHERE o_w_id >= {1} AND o_w_id <= {2} GROUP BY o_w_id, o_d_id) AS o "
        "LEFT JOIN (SELECT no_w_id, no_d_id, COUNT(*) AS new_order_cnt FROM {3} "
        "WHERE no_w_id >= {1} AND no_w_id <= {2} GROUP BY no_w_id, no_d_id) AS n "
        "ON o.o_w_id = n.no_w_id AND o.o_d_id = n.no_d_id "
        "WHERE (COALESCE(o.order_cnt, 0) - COALESCE(n.new_order_cnt, 0)) != {4} "
        "UNION ALL "
        "SELECT n2.no_w_id, n2.no_d_id FROM (SELECT no_w_id, no_d_id, COUNT(*) AS new_order_cnt FROM {3} "
        "WHERE no_w_id >= {1} AND no_w_id <= {2} GROUP BY no_w_id, no_d_id) AS n2 "
        "LEFT JOIN (SELECT o_w_id, o_d_id, COUNT(*) AS order_cnt FROM {0} "
        "WHERE o_w_id >= {1} AND o_w_id <= {2} GROUP BY o_w_id, o_d_id) AS o2 "
        "ON o2.o_w_id = n2.no_w_id AND o2.o_d_id = n2.no_d_id "
        "WHERE o2.o_w_id IS NULL"
        ") sub LIMIT 1",
        TABLE_OORDER, startWh, endWh, TABLE_NEW_ORDER,
        FIRST_UNPROCESSED_O_ID - 1),
        fmt::format("3.3.2.11 w_id [{},{}]", startWh, endWh));
}

void ConsistencyCheck33212(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT c.c_w_id FROM {0} AS c LEFT JOIN ("
        "SELECT o.o_w_id AS w_id, o.o_d_id AS d_id, o.o_c_id AS c_id, SUM(ol.ol_amount) AS ol_sum "
        "FROM {1} AS o JOIN {2} AS ol ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id "
        "WHERE ol.ol_delivery_d IS NOT NULL AND o.o_w_id >= {3} AND o.o_w_id <= {4} GROUP BY o.o_w_id, o.o_d_id, o.o_c_id) AS l "
        "ON c.c_w_id = l.w_id AND c.c_d_id = l.d_id AND c.c_id = l.c_id "
        "WHERE c.c_w_id >= {3} AND c.c_w_id <= {4} "
        "AND NOT ((c.c_balance + c.c_ytd_payment) <=> COALESCE(l.ol_sum, 0)) LIMIT 1",
        TABLE_CUSTOMER, TABLE_OORDER, TABLE_ORDER_LINE, startWh, endWh),
        fmt::format("3.3.2.12 w_id [{},{}]", startWh, endWh));
}

void PostImportCheckNextOrderId(TObConnection& conn) {
    CheckNoRows(conn, fmt::format(
        "SELECT d_w_id FROM {} WHERE d_next_o_id != {} LIMIT 1",
        TABLE_DISTRICT, CUSTOMERS_PER_DISTRICT + 1));
}

void PostImportCheckWarehouseYtd(TObConnection& conn) {
    CheckNoRows(conn, fmt::format(
        "SELECT w_id FROM {} WHERE NOT (w_ytd <=> {}) LIMIT 1",
        TABLE_WAREHOUSE, WAREHOUSE_INITIAL_YTD.ToString()));
}

void PostImportCheckDistrictYtd(TObConnection& conn) {
    CheckNoRows(conn, fmt::format(
        "SELECT d_w_id FROM {} WHERE NOT (d_ytd <=> {}) LIMIT 1",
        TABLE_DISTRICT, DISTRICT_INITIAL_YTD.ToString()));
}

void PostImportCheckNoCarriers(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT o_w_id FROM {} WHERE o_w_id >= {} AND o_w_id <= {} "
        "AND o_id >= {} AND o_carrier_id IS NOT NULL LIMIT 1",
        TABLE_OORDER, startWh, endWh, FIRST_UNPROCESSED_O_ID),
        fmt::format("Unprocessed orders must have NULL O_CARRIER_ID after import (w_id [{},{}])",
                    startWh, endWh));
}

void PostImportCheckCarrierRange(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT o_w_id FROM {} WHERE o_w_id >= {} AND o_w_id <= {} AND o_id < {} "
        "AND (o_carrier_id IS NULL OR o_carrier_id < 1 OR o_carrier_id > 10) LIMIT 1",
        TABLE_OORDER, startWh, endWh, FIRST_UNPROCESSED_O_ID),
        fmt::format("Delivered orders must have O_CARRIER_ID in [1..10] after import (w_id [{},{}])",
                    startWh, endWh));
}

void PostImportCheckNoDeliveryDates(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT ol_w_id FROM {} WHERE ol_w_id >= {} AND ol_w_id <= {} "
        "AND ol_o_id >= {} AND ol_delivery_d IS NOT NULL LIMIT 1",
        TABLE_ORDER_LINE, startWh, endWh, FIRST_UNPROCESSED_O_ID),
        fmt::format("Unprocessed order lines must have NULL OL_DELIVERY_D after import (w_id [{},{}])",
                    startWh, endWh));
}

void PostImportCheckDeliveryEqualsEntry(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT ol.ol_w_id FROM {} AS ol JOIN {} AS o "
        "ON o.o_w_id = ol.ol_w_id AND o.o_d_id = ol.ol_d_id AND o.o_id = ol.ol_o_id "
        "WHERE ol.ol_w_id >= {} AND ol.ol_w_id <= {} AND ol.ol_o_id < {} "
        "AND (ol.ol_delivery_d IS NULL OR ol.ol_delivery_d <> o.o_entry_d) LIMIT 1",
        TABLE_ORDER_LINE, TABLE_OORDER, startWh, endWh, FIRST_UNPROCESSED_O_ID),
        fmt::format(
            "Delivered order lines must have OL_DELIVERY_D = O_ENTRY_D after import (w_id [{},{}])",
            startWh, endWh));
}

void PostImportCheckDeliveredAmountZero(TObConnection& conn, int startWh, int endWh) {
    CheckNoRows(conn, fmt::format(
        "SELECT ol_w_id FROM {} WHERE ol_w_id >= {} AND ol_w_id <= {} "
        "AND ol_o_id < {} AND NOT (ol_amount <=> 0.00) LIMIT 1",
        TABLE_ORDER_LINE, startWh, endWh, FIRST_UNPROCESSED_O_ID),
        fmt::format("Delivered order lines must have OL_AMOUNT = 0.00 after import (w_id [{},{}])",
                    startWh, endWh));
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

struct TCheckJob {
    std::string Id;
    // All tasks must succeed for the check to pass (used for warehouse-range chunks).
    std::vector<std::function<void(TObConnection&)>> Tasks;
};

void AddSingleJob(std::vector<TCheckJob>& jobs, std::string id,
                  std::function<void(TObConnection&)> task) {
    TCheckJob job;
    job.Id = std::move(id);
    job.Tasks.push_back(std::move(task));
    jobs.push_back(std::move(job));
}

void AddRangedJob(std::vector<TCheckJob>& jobs, std::string id, int warehouseCount, int rangeSize,
                  auto&& rangeFn) {
    TCheckJob job;
    job.Id = std::move(id);
    for (int startWh = 1; startWh <= warehouseCount; startWh += rangeSize) {
        const int endWh = std::min(startWh + rangeSize - 1, warehouseCount);
        job.Tasks.push_back([rangeFn, startWh, endWh](TObConnection& conn) {
            rangeFn(conn, startWh, endWh);
        });
    }
    jobs.push_back(std::move(job));
}

std::unique_ptr<TObConnection> OpenCheckConnection(
    const std::string& connectionString, const std::string& path)
{
    auto conn = ConnectToTargetDatabase(ConfigWithPath(connectionString, path));
    conn->ConfigureBulkLoadSession();
    return conn;
}

// Runs check jobs using up to `concurrency` dedicated OceanBase sessions.
// SQL predicates are unchanged; only scheduling is parallel.
// Catalog ids run one at a time (specification §9.2) so Checking … [OK]/[Failed]
// is printed as soon as that job's warehouse chunks finish. Chunks of the
// current id still share the session pool.
void RunCheckJobs(
    TCheckReport& report,
    const std::string& connectionString,
    const std::string& path,
    int concurrency,
    std::vector<TCheckJob>& jobs,
    bool print)
{
    if (jobs.empty()) {
        return;
    }

    size_t maxTasks = 0;
    for (const auto& job : jobs) {
        maxTasks = std::max(maxTasks, job.Tasks.size());
    }
    if (maxTasks == 0) {
        return;
    }

    const size_t poolSize = concurrency <= 1
        ? 1
        : std::min(static_cast<size_t>(concurrency), maxTasks);

    std::vector<std::unique_ptr<TObConnection>> conns;
    conns.reserve(poolSize);
    for (size_t i = 0; i < poolSize; ++i) {
        conns.push_back(OpenCheckConnection(connectionString, path));
    }

    struct TJobOutcome {
        std::atomic<bool> Failed{false};
        std::mutex DetailMutex;
        std::string Detail;
    };

    for (auto& job : jobs) {
        if (job.Tasks.empty()) {
            RecordResult(report, job.Id, ECheckStatus::Passed, {}, print);
            continue;
        }

        const size_t workers = std::min(poolSize, job.Tasks.size());
        TJobOutcome outcome;
        std::atomic<size_t> nextTask{0};

        auto workerFn = [&](size_t workerIndex) {
            auto& conn = *conns[workerIndex];
            for (;;) {
                const size_t idx = nextTask.fetch_add(1, std::memory_order_relaxed);
                if (idx >= job.Tasks.size()) {
                    break;
                }
                // Skip remaining chunks once the check has already failed.
                if (outcome.Failed.load(std::memory_order_relaxed)) {
                    continue;
                }
                try {
                    job.Tasks[idx](conn);
                } catch (const std::exception& ex) {
                    bool expected = false;
                    if (outcome.Failed.compare_exchange_strong(expected, true)) {
                        std::lock_guard lock(outcome.DetailMutex);
                        outcome.Detail = ex.what();
                    }
                }
            }
        };

        if (workers == 1) {
            workerFn(0);
        } else {
            std::vector<std::thread> threads;
            threads.reserve(workers);
            for (size_t i = 0; i < workers; ++i) {
                threads.emplace_back(workerFn, i);
            }
            for (auto& t : threads) {
                t.join();
            }
        }

        if (outcome.Failed.load()) {
            std::string detail;
            {
                std::lock_guard lock(outcome.DetailMutex);
                detail = outcome.Detail;
            }
            RecordResult(report, job.Id, ECheckStatus::Failed, detail, print);
        } else {
            RecordResult(report, job.Id, ECheckStatus::Passed, {}, print);
        }
    }
}

std::vector<TCheckJob> BuildCardinalityJobs(const TCheckRequest& request, bool afterImport) {
    std::vector<TCheckJob> jobs;
    const int wh = request.WarehouseCount;
    AddSingleJob(jobs, "cardinality.warehouse",
                 [wh](TObConnection& conn) { BaseCheckWarehouseTable(conn, wh); });
    AddSingleJob(jobs, "cardinality.district",
                 [wh](TObConnection& conn) { BaseCheckDistrictTable(conn, wh); });
    AddRangedJob(jobs, "cardinality.customer", wh, kWarehouseCheckRange, BaseCheckCustomerTable);
    AddSingleJob(jobs, "cardinality.item",
                 [](TObConnection& conn) { BaseCheckItemTable(conn); });
    AddRangedJob(jobs, "cardinality.stock", wh, kWarehouseCheckRange, BaseCheckStockTable);
    if (afterImport) {
        AddRangedJob(jobs, "cardinality.oorder", wh, kWarehouseCheckRange, BaseCheckOorderTable);
        AddRangedJob(jobs, "cardinality.new_order", wh, kWarehouseCheckRange, BaseCheckNewOrderTable);
        AddRangedJob(jobs, "cardinality.order_line", wh, kWarehouseCheckRange, BaseCheckOrderLineTable);
        AddRangedJob(jobs, "cardinality.history", wh, kWarehouseCheckRange, BaseCheckHistoryTable);
    }
    return jobs;
}

std::vector<TCheckJob> BuildConsistencyJobs(const TCheckRequest& request, bool afterImport) {
    std::vector<TCheckJob> jobs;
    const int wh = request.WarehouseCount;

    AddSingleJob(jobs, "consistency.3.3.2.1",
                 [](TObConnection& conn) { ConsistencyCheck3321(conn); });
    AddRangedJob(jobs, "consistency.3.3.2.2", wh, kWarehouseCheckRange, ConsistencyCheck3322);
    AddRangedJob(jobs, "consistency.3.3.2.3", wh, kWarehouseCheckRange, ConsistencyCheck3323);
    AddRangedJob(jobs, "consistency.3.3.2.4", wh, kWarehouseCheckRange, ConsistencyCheck3324);
    AddRangedJob(jobs, "consistency.3.3.2.5", wh, kWarehouseCheckRange, ConsistencyCheck3325);
    AddRangedJob(jobs, "consistency.3.3.2.6", wh, kWarehouseCheckRange, ConsistencyCheck3326);
    AddRangedJob(jobs, "consistency.3.3.2.7", wh, kWarehouseCheckRange, ConsistencyCheck3327);
    AddRangedJob(jobs, "consistency.3.3.2.8", wh, kWarehouseCheckRange, ConsistencyCheck3328);
    AddRangedJob(jobs, "consistency.3.3.2.9", wh, kWarehouseCheckRange, ConsistencyCheck3329);
    AddRangedJob(jobs, "consistency.3.3.2.10", wh, kWarehouseCheckRange, ConsistencyCheck33210);
    AddRangedJob(jobs, "consistency.3.3.2.12", wh, kWarehouseCheckRange, ConsistencyCheck33212);

    if (afterImport) {
        AddRangedJob(jobs, "consistency.3.3.2.11", wh, kWarehouseCheckRange, ConsistencyCheck33211);
        AddSingleJob(jobs, "post_import.d_next_o_id",
                     [](TObConnection& conn) { PostImportCheckNextOrderId(conn); });
        AddSingleJob(jobs, "post_import.w_ytd",
                     [](TObConnection& conn) { PostImportCheckWarehouseYtd(conn); });
        AddSingleJob(jobs, "post_import.d_ytd",
                     [](TObConnection& conn) { PostImportCheckDistrictYtd(conn); });
        AddRangedJob(jobs, "post_import.o_carrier_id", wh, kWarehouseCheckRange,
                     PostImportCheckNoCarriers);
        AddRangedJob(jobs, "post_import.o_carrier_id_range", wh, kWarehouseCheckRange,
                     PostImportCheckCarrierRange);
        AddRangedJob(jobs, "post_import.ol_delivery_d", wh, kWarehouseCheckRange,
                     PostImportCheckNoDeliveryDates);
        AddRangedJob(jobs, "post_import.ol_delivery_eq_entry", wh, kWarehouseCheckRange,
                     PostImportCheckDeliveryEqualsEntry);
        AddRangedJob(jobs, "post_import.ol_amount_delivered", wh, kWarehouseCheckRange,
                     PostImportCheckDeliveredAmountZero);
    }
    return jobs;
}

} // namespace

TCheckReport RunObChecks(const std::string& connectionString, const TCheckRequest& request) {
    TCheckReport report;
    report.RunId = request.RunId;
    report.Instance = request.Instance;
    report.Phase = request.Phase == ECheckPhase::AfterImport ? "after-import" : "after-run";
    report.WarehouseCount = request.WarehouseCount;

    const bool print = true;
    const bool afterImport = request.Phase == ECheckPhase::AfterImport;
    const int concurrency = request.CheckConcurrency <= 1 ? 1 : request.CheckConcurrency;

    if (request.WarehouseCount <= 0) {
        RecordResult(report, "cardinality.warehouse", ECheckStatus::Error,
                     "Zero warehouses specified", print);
        return report;
    }

    try {
        if (concurrency > 1) {
            std::cout << "Running checks with concurrency=" << concurrency << std::endl;
        }

        auto cardinalityJobs = BuildCardinalityJobs(request, afterImport);
        RunCheckJobs(report, connectionString, request.Path, concurrency, cardinalityJobs, print);

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

        auto consistencyJobs = BuildConsistencyJobs(request, afterImport);
        RunCheckJobs(report, connectionString, request.Path, concurrency, consistencyJobs, print);
    } catch (const std::exception& ex) {
        RecordResult(report, "connection", ECheckStatus::Error, ex.what(), print);
    }

    if (report.Ok()) {
        std::cout << "Everything is good!" << std::endl;
    }
    return report;
}

void CheckSync(
    const std::string& connectionString,
    int warehouseCount,
    bool afterImport,
    const std::string& path,
    int checkConcurrency)
{
    TCheckRequest req;
    req.WarehouseCount = warehouseCount;
    req.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterRun;
    req.Path = path;
    req.CheckConcurrency = checkConcurrency <= 1 ? 1 : checkConcurrency;
    auto report = RunObChecks(connectionString, req);
    if (!report.Ok()) {
        throw std::runtime_error(fmt::format("{} checks failed", report.FailedCount + report.ErrorCount));
    }
}

TObCheckAdapter::TObCheckAdapter(std::string connectionString)
    : ConnectionString_(std::move(connectionString))
{}

TCheckReport TObCheckAdapter::Run(const TCheckRequest& request) {
    return RunObChecks(ConnectionString_, request);
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
        const std::string connection = BuildObConnectionString(doc);
        CheckDbForRun(connection, doc.ScaleWarehouses, doc.Path);

        TCheckRequest req;
        req.WarehouseCount = doc.ScaleWarehouses;
        req.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterRun;
        req.Path = doc.Path;
        req.RunId = doc.RunId;
        req.Instance = instance;
        req.CheckConcurrency = checkConcurrency <= 1 ? 1 : checkConcurrency;

        TObCheckAdapter adapter(connection);
        const auto report = adapter.Run(req);

        const std::string checksDir = doc.RunDir + "/checks";
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

#include "ob_batch.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace NTpcc {

namespace {

struct TPairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& pair) const {
        return std::hash<T1>()(pair.first) ^ std::hash<T2>()(pair.second);
    }
};

void RequireBatchSize(size_t n, const char* what) {
    if (n == 0 || n > ObBatchMaxRows) {
        throw std::invalid_argument(
            std::string(what) + ": n must be in 1.." + std::to_string(ObBatchMaxRows));
    }
}

std::string RepeatJoined(const char* token, size_t n, const char* sep) {
    std::string out;
    const size_t tokenLen = std::char_traits<char>::length(token);
    const size_t sepLen = std::char_traits<char>::length(sep);
    out.reserve(n * (tokenLen + sepLen));
    for (size_t i = 0; i < n; ++i) {
        if (i) {
            out += sep;
        }
        out += token;
    }
    return out;
}

} // namespace

std::vector<TObStockBatchItem> AggregateObStockUpdates(const std::vector<TSemanticOp>& ops) {
    std::unordered_map<std::pair<int, int>, TObStockBatchItem, TPairHash> byKey;
    std::vector<std::pair<int, int>> order;
    byKey.reserve(ops.size());
    order.reserve(ops.size());

    for (const auto& op : ops) {
        const auto* p = std::get_if<TUpdateStock>(&op);
        if (!p) {
            continue;
        }
        const auto key = std::make_pair(p->WarehouseID, p->ItemID);
        auto [it, inserted] = byKey.try_emplace(key);
        if (inserted) {
            order.push_back(key);
            it->second.WarehouseID = p->WarehouseID;
            it->second.ItemID = p->ItemID;
        }
        auto& row = it->second;
        row.NewQuantity = p->NewQuantity;
        row.OrderedQuantity += p->OrderedQuantity;
        row.RemoteIncrement += p->RemoteIncrement;
        row.LineCount += 1;
    }

    std::vector<TObStockBatchItem> out;
    out.reserve(order.size());
    for (const auto& key : order) {
        out.push_back(byKey[key]);
    }
    return out;
}

std::vector<int> UniqueItemIds(const std::vector<int>& ids) {
    std::vector<int> out = ids;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<TStockKey> UniqueSortedStockKeys(const std::vector<TStockKey>& keys) {
    std::vector<TStockKey> out = keys;
    std::sort(out.begin(), out.end(), [](const TStockKey& a, const TStockKey& b) {
        if (a.WarehouseID != b.WarehouseID) {
            return a.WarehouseID < b.WarehouseID;
        }
        return a.ItemID < b.ItemID;
    });
    out.erase(
        std::unique(
            out.begin(),
            out.end(),
            [](const TStockKey& a, const TStockKey& b) {
                return a.WarehouseID == b.WarehouseID && a.ItemID == b.ItemID;
            }),
        out.end());
    return out;
}

std::string BuildObGetItemsSql(size_t n) {
    RequireBatchSize(n, "BuildObGetItemsSql");
    return "SELECT i_id, i_price, i_name, i_data FROM item WHERE i_id IN ("
        + RepeatJoined("?", n, ",") + ")";
}

std::string BuildObGetStocksForUpdateSql(size_t n) {
    RequireBatchSize(n, "BuildObGetStocksForUpdateSql");
    return "SELECT s_w_id, s_i_id, s_quantity, s_ytd, s_order_cnt, s_remote_cnt, s_data, "
           "s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, "
           "s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 "
           "FROM stock WHERE (s_w_id, s_i_id) IN ("
        + RepeatJoined("(?,?)", n, ",")
        + ") ORDER BY s_w_id, s_i_id FOR UPDATE";
}

std::string BuildObStockUpdateBatchSql(size_t n) {
    RequireBatchSize(n, "BuildObStockUpdateBatchSql");
    std::string sql = "UPDATE stock s INNER JOIN (";
    for (size_t i = 0; i < n; ++i) {
        if (i == 0) {
            sql += "SELECT ? AS w_id, ? AS i_id, ? AS qty, ? AS ytd_inc, ? AS oc, ? AS rc";
        } else {
            sql += " UNION ALL SELECT ?,?,?,?,?,?";
        }
    }
    sql += ") u ON s.s_w_id = u.w_id AND s.s_i_id = u.i_id"
           " SET s.s_quantity = u.qty,"
           " s.s_ytd = s.s_ytd + u.ytd_inc,"
           " s.s_order_cnt = s.s_order_cnt + u.oc,"
           " s.s_remote_cnt = s.s_remote_cnt + u.rc";
    return sql;
}

std::string BuildObOrderLineInsertSql(size_t n) {
    RequireBatchSize(n, "BuildObOrderLineInsertSql");
    return "INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, "
           "ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES "
        + RepeatJoined("(?,?,?,?,?,?,?,?,?)", n, ",");
}

void RequireDeliverySize(size_t n, const char* what) {
    if (n == 0 || n > static_cast<size_t>(DISTRICT_COUNT)) {
        throw std::invalid_argument(
            std::string(what) + ": n must be in 1.." + std::to_string(DISTRICT_COUNT));
    }
}

std::string IntPairList(const std::vector<TObDeliveryOrderKey>& orders) {
    std::string out;
    out.reserve(orders.size() * 12);
    for (size_t i = 0; i < orders.size(); ++i) {
        if (i) {
            out += ',';
        }
        out += '(';
        out += std::to_string(orders[i].DistrictID);
        out += ',';
        out += std::to_string(orders[i].OrderID);
        out += ')';
    }
    return out;
}

std::string BuildObPaymentLocationSql(int warehouseId, int districtId, std::string_view amount) {
    const auto w = std::to_string(warehouseId);
    const auto d = std::to_string(districtId);
    std::string amt(amount);
    return "UPDATE warehouse w INNER JOIN district d ON d.d_w_id = w.w_id AND d.d_id = " + d
        + " SET w.w_ytd = w.w_ytd + " + amt + ", d.d_ytd = d.d_ytd + " + amt
        + " WHERE w.w_id = " + w
        + "; SELECT w.w_name, w.w_street_1, w.w_street_2, w.w_city, w.w_state, w.w_zip,"
          " d.d_name, d.d_street_1, d.d_street_2, d.d_city, d.d_state, d.d_zip"
          " FROM warehouse w INNER JOIN district d ON d.d_w_id = w.w_id AND d.d_id = "
        + d + " WHERE w.w_id = " + w;
}

std::string BuildObOldestNewOrdersSql(int warehouseId) {
    const auto w = std::to_string(warehouseId);
    std::string sql;
    sql.reserve(static_cast<size_t>(DISTRICT_COUNT) * 120);
    for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
        if (d != DISTRICT_LOW_ID) {
            sql += ';';
        }
        sql += "SELECT no_o_id FROM new_order WHERE no_w_id = " + w + " AND no_d_id = "
            + std::to_string(d) + " ORDER BY no_o_id ASC LIMIT 1 FOR UPDATE";
    }
    return sql;
}

std::string BuildObDeliveryOrderInfoSql(
    int warehouseId,
    const std::vector<TObDeliveryOrderKey>& orders)
{
    RequireDeliverySize(orders.size(), "BuildObDeliveryOrderInfoSql");
    const auto w = std::to_string(warehouseId);
    const auto keys = IntPairList(orders);
    return "SELECT o_d_id, o_id, o_c_id FROM oorder WHERE o_w_id = " + w
        + " AND (o_d_id, o_id) IN (" + keys + ")"
        + "; SELECT ol_d_id, ol_o_id, ol_amount FROM order_line WHERE ol_w_id = " + w
        + " AND (ol_d_id, ol_o_id) IN (" + keys + ")";
}

std::string BuildObDeliveryCompleteSql(
    int warehouseId,
    int carrierId,
    const std::vector<TObDeliveryOrderKey>& orders)
{
    RequireDeliverySize(orders.size(), "BuildObDeliveryCompleteSql");
    const auto w = std::to_string(warehouseId);
    const auto keys = IntPairList(orders);
    return "DELETE FROM new_order WHERE no_w_id = " + w + " AND (no_d_id, no_o_id) IN (" + keys
        + "); UPDATE oorder SET o_carrier_id = " + std::to_string(carrierId)
        + " WHERE o_w_id = " + w + " AND (o_d_id, o_id) IN (" + keys
        + "); UPDATE order_line SET ol_delivery_d = CURRENT_TIMESTAMP WHERE ol_w_id = " + w
        + " AND (ol_d_id, ol_o_id) IN (" + keys + ")";
}

std::string BuildObDeliveryApplySql(size_t n) {
    RequireDeliverySize(n, "BuildObDeliveryApplySql");
    std::string sql = "UPDATE customer c INNER JOIN (";
    for (size_t i = 0; i < n; ++i) {
        if (i == 0) {
            sql += "SELECT ? AS w_id, ? AS d_id, ? AS c_id, ? AS amount";
        } else {
            sql += " UNION ALL SELECT ?,?,?,?";
        }
    }
    sql += ") u ON c.c_w_id = u.w_id AND c.c_d_id = u.d_id AND c.c_id = u.c_id"
           " SET c.c_balance = c.c_balance + u.amount,"
           " c.c_delivery_cnt = c.c_delivery_cnt + 1";
    return sql;
}

std::string BuildObDeliveryFinishSql(const TApplyDeliveryToCustomer& apply) {
    return "UPDATE customer SET c_balance = c_balance + " + apply.Amount.ToString()
        + ", c_delivery_cnt = c_delivery_cnt + 1 WHERE c_w_id = "
        + std::to_string(apply.WarehouseID) + " AND c_d_id = "
        + std::to_string(apply.DistrictID) + " AND c_id = "
        + std::to_string(apply.CustomerID) + "; COMMIT";
}

std::string BuildObPaymentFinishSql(
    const TUpdateCustomerPayment& update,
    const TInsertPaymentHistory& history,
    const std::string& quotedCustomerData,
    const std::string& quotedHistoryData)
{
    std::string sql = "UPDATE customer SET c_balance = " + update.NewBalance.ToString()
        + ", c_ytd_payment = " + update.NewYtdPayment.ToString()
        + ", c_payment_cnt = " + std::to_string(update.NewPaymentCount);
    if (update.UpdateData) {
        sql += ", c_data = " + quotedCustomerData;
    }
    sql += " WHERE c_w_id = " + std::to_string(update.WarehouseID)
        + " AND c_d_id = " + std::to_string(update.DistrictID)
        + " AND c_id = " + std::to_string(update.CustomerID)
        + "; INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)"
          " VALUES ("
        + std::to_string(history.CustomerID) + ", "
        + std::to_string(history.CustomerDistrictID) + ", "
        + std::to_string(history.CustomerWarehouseID) + ", "
        + std::to_string(history.PaymentDistrictID) + ", "
        + std::to_string(history.PaymentWarehouseID) + ", CURRENT_TIMESTAMP, "
        + history.Amount.ToString() + ", " + quotedHistoryData
        + "); COMMIT";
    return sql;
}

} // namespace NTpcc

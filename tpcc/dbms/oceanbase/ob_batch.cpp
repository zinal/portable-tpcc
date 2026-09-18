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

} // namespace NTpcc

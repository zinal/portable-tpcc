#include "ydb_batch.h"

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

} // anonymous

std::vector<TYdbStockBatchItem> AggregateYdbStockUpdates(const std::vector<TSemanticOp>& ops) {
    std::unordered_map<std::pair<int, int>, TYdbStockBatchItem, TPairHash> byKey;
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
        row.NewYtd = p->NewYtd;
        row.NewOrderCount = p->NewOrderCount;
        row.NewRemoteCount = p->NewRemoteCount;
    }

    std::vector<TYdbStockBatchItem> out;
    out.reserve(order.size());
    for (const auto& key : order) {
        out.push_back(byKey[key]);
    }
    return out;
}

} // namespace NTpcc

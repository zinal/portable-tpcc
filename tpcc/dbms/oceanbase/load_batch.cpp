#include "load_batch.h"

#include <constants.h>
#include <log.h>
#include <populate.h>

#include <fmt/format.h>

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace NTpcc {

namespace {

using TCell = std::optional<std::string>;
using TRow = std::vector<TCell>;

std::string FormatUnixTimestamp(int64_t unixSeconds) {
    time_t t = static_cast<time_t>(unixSeconds);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

TCell Cell(int v) {
    return std::to_string(v);
}

TCell Cell(const std::string& v) {
    return v;
}

TCell Cell(std::optional<int> v) {
    if (!v) return std::nullopt;
    return std::to_string(*v);
}

TCell Cell(std::optional<std::string> v) {
    return v;
}

TPutBatchResult OkResult() {
    TPutBatchResult r;
    r.Outcome = EPutBatchOutcome::Completed;
    return r;
}

TPutBatchResult FailResult(const std::exception& ex) {
    TPutBatchResult r;
    r.Outcome = EPutBatchOutcome::Failed;
    r.Message = ex.what();
    return r;
}

size_t BatchSize(int batchRows) {
    return batchRows > 0 ? static_cast<size_t>(batchRows) : 200;
}

void InsertRows(
    TObConnection& conn,
    const std::string& table,
    const std::vector<std::string>& columns,
    const std::vector<TRow>& rows,
    const std::string& suffix = {})
{
    if (rows.empty()) {
        return;
    }

    std::string sql = "INSERT INTO " + QuoteIdent(table) + " (";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i) sql += ',';
        sql += QuoteIdent(columns[i]);
    }
    sql += ") VALUES ";

    TObParams params;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r) sql += ',';
        sql += '(';
        if (rows[r].size() != columns.size()) {
            throw std::runtime_error("row column count mismatch for " + table);
        }
        for (size_t c = 0; c < rows[r].size(); ++c) {
            if (c) sql += ',';
            sql += '?';
            if (rows[r][c]) {
                params(*rows[r][c]);
            } else {
                params(nullptr);
            }
        }
        sql += ')';
    }
    sql += suffix;
    conn.Execute(sql, params);
}

template <typename TEmit>
void EmitBatches(
    TObConnection& conn,
    const std::string& table,
    const std::vector<std::string>& columns,
    size_t batchSize,
    TEmit emit,
    const std::string& suffix = {})
{
    std::vector<TRow> batch;
    batch.reserve(batchSize);
    auto add = [&](TRow row) {
        batch.push_back(std::move(row));
        if (batch.size() >= batchSize) {
            InsertRows(conn, table, columns, batch, suffix);
            batch.clear();
        }
    };
    emit(add);
    InsertRows(conn, table, columns, batch, suffix);
}

void InsertWarehouse(TObConnection& conn, uint64_t seed, int wh) {
    auto row = NGenerator::GenerateWarehouse(seed, wh);
    InsertRows(conn, "warehouse",
        {"w_id", "w_ytd", "w_tax", "w_name", "w_street_1", "w_street_2", "w_city", "w_state", "w_zip"},
        {{
            Cell(row.Id), Cell(row.Ytd.ToString()), Cell(row.Tax.ToString()), Cell(row.Name),
            Cell(row.Street1), Cell(row.Street2), Cell(row.City), Cell(row.State), Cell(row.Zip),
        }});
}

void InsertDistricts(TObConnection& conn, uint64_t seed, int wh) {
    std::vector<TRow> rows;
    for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
        auto row = NGenerator::GenerateDistrict(seed, wh, d);
        rows.push_back({
            Cell(row.WarehouseId), Cell(row.Id), Cell(row.Ytd.ToString()), Cell(row.Tax.ToString()),
            Cell(row.NextOrderId), Cell(row.Name), Cell(row.Street1), Cell(row.Street2),
            Cell(row.City), Cell(row.State), Cell(row.Zip),
        });
    }
    InsertRows(conn, "district",
        {"d_w_id", "d_id", "d_ytd", "d_tax", "d_next_o_id", "d_name",
         "d_street_1", "d_street_2", "d_city", "d_state", "d_zip"},
        rows);
}

void InsertStock(TObConnection& conn, uint64_t seed, int wh, int batchRows) {
    EmitBatches(conn, "stock",
        {"s_w_id", "s_i_id", "s_quantity", "s_ytd", "s_order_cnt", "s_remote_cnt",
         "s_data", "s_dist_01", "s_dist_02", "s_dist_03", "s_dist_04", "s_dist_05",
         "s_dist_06", "s_dist_07", "s_dist_08", "s_dist_09", "s_dist_10"},
        BatchSize(batchRows),
        [&](auto add) {
            for (int itemId = 1; itemId <= ITEM_COUNT; ++itemId) {
                auto row = NGenerator::GenerateStock(seed, wh, itemId);
                add({
                    Cell(row.WarehouseId), Cell(row.ItemId), Cell(row.Quantity), Cell(row.Ytd.ToString()),
                    Cell(row.OrderCount), Cell(row.RemoteCount), Cell(row.Data),
                    Cell(row.Dist[0]), Cell(row.Dist[1]), Cell(row.Dist[2]), Cell(row.Dist[3]), Cell(row.Dist[4]),
                    Cell(row.Dist[5]), Cell(row.Dist[6]), Cell(row.Dist[7]), Cell(row.Dist[8]), Cell(row.Dist[9]),
                });
            }
        });
}

void InsertCustomers(TObConnection& conn, uint64_t seed, int wh, int district, int batchRows) {
    EmitBatches(conn, "customer",
        {"c_w_id", "c_d_id", "c_id", "c_discount", "c_credit", "c_last", "c_first",
         "c_credit_lim", "c_balance", "c_ytd_payment", "c_payment_cnt", "c_delivery_cnt",
         "c_street_1", "c_street_2", "c_city", "c_state", "c_zip", "c_phone",
         "c_since", "c_middle", "c_data"},
        BatchSize(batchRows),
        [&](auto add) {
            for (int cid = C_FIRST_CUSTOMER_ID; cid <= CUSTOMERS_PER_DISTRICT; ++cid) {
                auto row = NGenerator::GenerateCustomer(seed, wh, district, cid);
                add({
                    Cell(row.WarehouseId), Cell(row.DistrictId), Cell(row.Id), Cell(row.Discount.ToString()),
                    Cell(row.Credit), Cell(row.Last), Cell(row.First), Cell(row.CreditLimit.ToString()),
                    Cell(row.Balance.ToString()), Cell(row.YtdPayment.ToString()), Cell(row.PaymentCount),
                    Cell(row.DeliveryCount), Cell(row.Street1), Cell(row.Street2), Cell(row.City),
                    Cell(row.State), Cell(row.Zip), Cell(row.Phone), Cell(FormatUnixTimestamp(row.SinceUnix)),
                    Cell(row.Middle), Cell(row.Data),
                });
            }
        });
}

void InsertHistory(TObConnection& conn, uint64_t seed, int wh, int district) {
    std::vector<TRow> rows;
    rows.reserve(CUSTOMERS_PER_DISTRICT);
    for (int cid = C_FIRST_CUSTOMER_ID; cid <= CUSTOMERS_PER_DISTRICT; ++cid) {
        auto row = NGenerator::GenerateHistory(seed, wh, district, cid);
        rows.push_back({
            Cell(row.CustomerId), Cell(row.CustomerDistrictId), Cell(row.CustomerWarehouseId),
            Cell(row.DistrictId), Cell(row.WarehouseId), Cell(FormatUnixTimestamp(row.DateUnix)),
            Cell(row.Amount.ToString()), Cell(row.Data),
        });
    }
    InsertRows(conn, "history",
        {"h_c_id", "h_c_d_id", "h_c_w_id", "h_d_id", "h_w_id", "h_date", "h_amount", "h_data"},
        rows);
}

void InsertOrders(TObConnection& conn, uint64_t seed, int wh, int district) {
    const auto customerIds = NGenerator::InitialOrderCustomerPermutation(seed, wh, district);
    std::vector<TRow> orders;
    std::vector<TRow> newOrders;
    std::vector<TRow> lines;
    orders.reserve(CUSTOMERS_PER_DISTRICT);
    newOrders.reserve(CUSTOMERS_PER_DISTRICT - FIRST_UNPROCESSED_O_ID + 1);

    for (int oid = 1; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
        int cid = customerIds[oid - 1];
        auto order = NGenerator::GenerateOrder(seed, wh, district, oid, cid);
        orders.push_back({
            Cell(order.WarehouseId), Cell(order.DistrictId), Cell(order.Id), Cell(order.CustomerId),
            Cell(order.CarrierId), Cell(order.OlCnt), Cell(order.AllLocal),
            Cell(FormatUnixTimestamp(order.EntryUnix)),
        });
        if (oid >= FIRST_UNPROCESSED_O_ID) {
            auto no = NGenerator::GenerateNewOrder(wh, district, oid);
            newOrders.push_back({Cell(no.WarehouseId), Cell(no.DistrictId), Cell(no.OrderId)});
        }

        const bool delivered = oid < FIRST_UNPROCESSED_O_ID;
        auto orderLines = NGenerator::GenerateOrderLines(
            seed, wh, district, oid, order.OlCnt,
            delivered ? std::optional<int64_t>(order.EntryUnix) : std::nullopt);
        for (const auto& line : orderLines) {
            std::optional<std::string> deliveryDate;
            if (line.DeliveryUnix) {
                deliveryDate = FormatUnixTimestamp(*line.DeliveryUnix);
            }
            lines.push_back({
                Cell(line.WarehouseId), Cell(line.DistrictId), Cell(line.OrderId), Cell(line.Number),
                Cell(line.ItemId), Cell(deliveryDate), Cell(line.Amount.ToString()),
                Cell(line.SupplyWarehouseId), Cell(line.Quantity), Cell(line.DistInfo),
            });
        }
    }

    InsertRows(conn, "oorder",
        {"o_w_id", "o_d_id", "o_id", "o_c_id", "o_carrier_id", "o_ol_cnt", "o_all_local", "o_entry_d"},
        orders);
    InsertRows(conn, "new_order", {"no_w_id", "no_d_id", "no_o_id"}, newOrders);
    InsertRows(conn, "order_line",
        {"ol_w_id", "ol_d_id", "ol_o_id", "ol_number", "ol_i_id", "ol_delivery_d",
         "ol_amount", "ol_supply_w_id", "ol_quantity", "ol_dist_info"},
        lines);
}

void DeleteWarehouseChildren(TObConnection& conn, int warehouseId) {
    conn.Execute("DELETE FROM order_line WHERE ol_w_id = ?", MakeParams(warehouseId));
    conn.Execute("DELETE FROM new_order WHERE no_w_id = ?", MakeParams(warehouseId));
    conn.Execute("DELETE FROM oorder WHERE o_w_id = ?", MakeParams(warehouseId));
    conn.Execute("DELETE FROM history WHERE h_w_id = ? OR h_c_w_id = ?", MakeParams(warehouseId, warehouseId));
    conn.Execute("DELETE FROM customer WHERE c_w_id = ?", MakeParams(warehouseId));
    conn.Execute("DELETE FROM district WHERE d_w_id = ?", MakeParams(warehouseId));
    conn.Execute("DELETE FROM stock WHERE s_w_id = ?", MakeParams(warehouseId));
    conn.Execute("DELETE FROM warehouse WHERE w_id = ?", MakeParams(warehouseId));
}

} // namespace

TPutBatchResult PutItemsIdempotent(
    TObConnection& conn,
    uint64_t seed,
    const std::string& runId,
    int batchRows)
{
    try {
        LOG_I("Idempotent load of " << ITEM_COUNT << " items (seed=" << seed
              << ", run_id=" << (runId.empty() ? "-" : runId)
              << ", batch_rows=" << batchRows << ")");

        conn.BeginRepeatableRead();
        const std::string suffix =
            " ON DUPLICATE KEY UPDATE "
            "i_name = VALUES(i_name), i_price = VALUES(i_price), "
            "i_data = VALUES(i_data), i_im_id = VALUES(i_im_id)";
        EmitBatches(conn, "item",
            {"i_id", "i_name", "i_price", "i_data", "i_im_id"},
            BatchSize(batchRows),
            [&](auto add) {
                for (int i = 1; i <= ITEM_COUNT; ++i) {
                    auto row = NGenerator::GenerateItem(seed, i);
                    add({
                        Cell(row.Id), Cell(row.Name), Cell(row.Price.ToString()),
                        Cell(row.Data), Cell(row.ImageId),
                    });
                }
            },
            suffix);
        conn.Commit();
        LOG_I("Items loaded (idempotent upsert)");
        return OkResult();
    } catch (const std::exception& ex) {
        conn.Rollback();
        LOG_E("PutItemsIdempotent failed: " << ex.what());
        return FailResult(ex);
    }
}

TPutBatchResult PutWarehouseIdempotent(
    TObConnection& conn,
    uint64_t seed,
    int warehouseId,
    const std::string& runId,
    int batchRows)
{
    try {
        LOG_D("Idempotent replace warehouse " << warehouseId << " (seed=" << seed
              << ", run_id=" << (runId.empty() ? "-" : runId)
              << ", batch_rows=" << batchRows << ")");

        conn.BeginRepeatableRead();
        DeleteWarehouseChildren(conn, warehouseId);
        InsertWarehouse(conn, seed, warehouseId);
        InsertDistricts(conn, seed, warehouseId);
        InsertStock(conn, seed, warehouseId, batchRows);
        for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
            InsertCustomers(conn, seed, warehouseId, d, batchRows);
            InsertHistory(conn, seed, warehouseId, d);
            InsertOrders(conn, seed, warehouseId, d);
        }
        conn.Commit();
        return OkResult();
    } catch (const std::exception& ex) {
        conn.Rollback();
        LOG_E("PutWarehouseIdempotent(w_id=" << warehouseId << ") failed: " << ex.what());
        return FailResult(ex);
    }
}

TObLoadAdapter::TObLoadAdapter(TObConnection& conn, uint64_t seed)
    : Conn_(conn)
    , Seed_(seed)
{}

TPutBatchResult TObLoadAdapter::PutBatch(
    const std::string& runId,
    const TLoadKeyRange& keyRange,
    const std::vector<std::string>& rows)
{
    if (!rows.empty()) {
        TPutBatchResult r;
        r.Outcome = EPutBatchOutcome::Failed;
        r.Message = "TObLoadAdapter does not yet accept pre-serialized rows; "
                    "pass an empty row list to regenerate from seed";
        return r;
    }

    if (keyRange.Table == TABLE_ITEM) {
        return PutItemsIdempotent(Conn_, Seed_, runId);
    }
    if (keyRange.Table == TABLE_WAREHOUSE) {
        if (keyRange.Begin >= keyRange.End) {
            TPutBatchResult r;
            r.Outcome = EPutBatchOutcome::Failed;
            r.Message = "empty warehouse key range";
            return r;
        }
        for (int64_t wh = keyRange.Begin; wh < keyRange.End; ++wh) {
            auto result = PutWarehouseIdempotent(Conn_, Seed_, static_cast<int>(wh), runId);
            if (result.Outcome != EPutBatchOutcome::Completed) {
                return result;
            }
        }
        return OkResult();
    }

    TPutBatchResult r;
    r.Outcome = EPutBatchOutcome::Failed;
    r.Message = fmt::format("unsupported PutBatch table '{}'", keyRange.Table);
    return r;
}

} // namespace NTpcc

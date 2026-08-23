#include "load_batch.h"
#include "ob_errors.h"

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
#include <string_view>
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

void InsertRowsChunk(
    TObConnection& conn,
    const std::string& table,
    const std::vector<std::string>& columns,
    const std::vector<TRow>& rows,
    size_t begin,
    size_t end,
    const std::string& suffix)
{
    std::string sql = "INSERT INTO " + QuoteIdent(table) + " (";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i) sql += ',';
        sql += QuoteIdent(columns[i]);
    }
    sql += ") VALUES ";

    TObParams params;
    for (size_t r = begin; r < end; ++r) {
        if (r > begin) sql += ',';
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
    batchSize = std::min(std::max<size_t>(1, batchSize), MaxRowsForColumns(columns.size()));
    std::vector<TRow> batch;
    batch.reserve(batchSize);
    auto flush = [&]() {
        if (batch.empty()) {
            return;
        }
        InsertRowsChunk(conn, table, columns, batch, 0, batch.size(), suffix);
        batch.clear();
    };
    auto add = [&](TRow row) {
        batch.push_back(std::move(row));
        if (batch.size() >= batchSize) {
            flush();
        }
    };
    emit(add);
    flush();
}

// Same shape as tpcc-oceanbase-cpp BulkInsert: one short TX per table.
template <typename TEmit>
void BulkInsert(
    TObConnection& conn,
    const std::string& table,
    const std::vector<std::string>& columns,
    int batchRows,
    TEmit emit,
    const std::string& suffix = {})
{
    conn.BeginRepeatableRead();
    try {
        EmitBatches(conn, table, columns, EffectiveObLoadBatchRows(columns.size(), batchRows), emit, suffix);
        conn.Commit();
    } catch (...) {
        conn.Rollback();
        throw;
    }
}

void InsertWarehouse(TObConnection& conn, uint64_t seed, int wh, int batchRows) {
    BulkInsert(conn, "warehouse",
        {"w_id", "w_ytd", "w_tax", "w_name", "w_street_1", "w_street_2", "w_city", "w_state", "w_zip"},
        batchRows,
        [&](auto add) {
            auto row = NGenerator::GenerateWarehouse(seed, wh);
            add({
                Cell(row.Id), Cell(row.Ytd.ToString()), Cell(row.Tax.ToString()), Cell(row.Name),
                Cell(row.Street1), Cell(row.Street2), Cell(row.City), Cell(row.State), Cell(row.Zip),
            });
        });
}

void InsertDistricts(TObConnection& conn, uint64_t seed, int wh, int batchRows) {
    BulkInsert(conn, "district",
        {"d_w_id", "d_id", "d_ytd", "d_tax", "d_next_o_id", "d_name",
         "d_street_1", "d_street_2", "d_city", "d_state", "d_zip"},
        batchRows,
        [&](auto add) {
            for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
                auto row = NGenerator::GenerateDistrict(seed, wh, d);
                add({
                    Cell(row.WarehouseId), Cell(row.Id), Cell(row.Ytd.ToString()), Cell(row.Tax.ToString()),
                    Cell(row.NextOrderId), Cell(row.Name), Cell(row.Street1), Cell(row.Street2),
                    Cell(row.City), Cell(row.State), Cell(row.Zip),
                });
            }
        });
}

void InsertStock(TObConnection& conn, uint64_t seed, int wh, int batchRows) {
    BulkInsert(conn, "stock",
        {"s_w_id", "s_i_id", "s_quantity", "s_ytd", "s_order_cnt", "s_remote_cnt",
         "s_data", "s_dist_01", "s_dist_02", "s_dist_03", "s_dist_04", "s_dist_05",
         "s_dist_06", "s_dist_07", "s_dist_08", "s_dist_09", "s_dist_10"},
        batchRows,
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
    BulkInsert(conn, "customer",
        {"c_w_id", "c_d_id", "c_id", "c_discount", "c_credit", "c_last", "c_first",
         "c_credit_lim", "c_balance", "c_ytd_payment", "c_payment_cnt", "c_delivery_cnt",
         "c_street_1", "c_street_2", "c_city", "c_state", "c_zip", "c_phone",
         "c_since", "c_middle", "c_data"},
        batchRows,
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

void InsertHistory(TObConnection& conn, uint64_t seed, int wh, int district, int batchRows) {
    BulkInsert(conn, "history",
        {"h_c_id", "h_c_d_id", "h_c_w_id", "h_d_id", "h_w_id", "h_date", "h_amount", "h_data"},
        batchRows,
        [&](auto add) {
            for (int cid = C_FIRST_CUSTOMER_ID; cid <= CUSTOMERS_PER_DISTRICT; ++cid) {
                auto row = NGenerator::GenerateHistory(seed, wh, district, cid);
                add({
                    Cell(row.CustomerId), Cell(row.CustomerDistrictId), Cell(row.CustomerWarehouseId),
                    Cell(row.DistrictId), Cell(row.WarehouseId), Cell(FormatUnixTimestamp(row.DateUnix)),
                    Cell(row.Amount.ToString()), Cell(row.Data),
                });
            }
        });
}

void InsertOrders(TObConnection& conn, uint64_t seed, int wh, int district, int batchRows) {
    const auto customerIds = NGenerator::InitialOrderCustomerPermutation(seed, wh, district);

    BulkInsert(conn, "oorder",
        {"o_w_id", "o_d_id", "o_id", "o_c_id", "o_carrier_id", "o_ol_cnt", "o_all_local", "o_entry_d"},
        batchRows,
        [&](auto add) {
            for (int oid = 1; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
                int cid = customerIds[oid - 1];
                auto order = NGenerator::GenerateOrder(seed, wh, district, oid, cid);
                add({
                    Cell(order.WarehouseId), Cell(order.DistrictId), Cell(order.Id), Cell(order.CustomerId),
                    Cell(order.CarrierId), Cell(order.OlCnt), Cell(order.AllLocal),
                    Cell(FormatUnixTimestamp(order.EntryUnix)),
                });
            }
        });

    BulkInsert(conn, "new_order",
        {"no_w_id", "no_d_id", "no_o_id"},
        batchRows,
        [&](auto add) {
            for (int oid = FIRST_UNPROCESSED_O_ID; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
                auto no = NGenerator::GenerateNewOrder(wh, district, oid);
                add({Cell(no.WarehouseId), Cell(no.DistrictId), Cell(no.OrderId)});
            }
        });

    BulkInsert(conn, "order_line",
        {"ol_w_id", "ol_d_id", "ol_o_id", "ol_number", "ol_i_id", "ol_delivery_d",
         "ol_amount", "ol_supply_w_id", "ol_quantity", "ol_dist_info"},
        batchRows,
        [&](auto add) {
            for (int oid = 1; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
                int cid = customerIds[oid - 1];
                auto order = NGenerator::GenerateOrder(seed, wh, district, oid, cid);
                const bool delivered = oid < FIRST_UNPROCESSED_O_ID;
                auto orderLines = NGenerator::GenerateOrderLines(
                    seed, wh, district, oid, order.OlCnt,
                    delivered ? std::optional<int64_t>(order.EntryUnix) : std::nullopt);
                for (const auto& line : orderLines) {
                    std::optional<std::string> deliveryDate;
                    if (line.DeliveryUnix) {
                        deliveryDate = FormatUnixTimestamp(*line.DeliveryUnix);
                    }
                    add({
                        Cell(line.WarehouseId), Cell(line.DistrictId), Cell(line.OrderId), Cell(line.Number),
                        Cell(line.ItemId), Cell(deliveryDate), Cell(line.Amount.ToString()),
                        Cell(line.SupplyWarehouseId), Cell(line.Quantity), Cell(line.DistInfo),
                    });
                }
            }
        });
}

void DeleteWarehouseChildren(TObConnection& conn, int warehouseId) {
    conn.BeginRepeatableRead();
    try {
        conn.Execute("DELETE FROM order_line WHERE ol_w_id = ?", MakeParams(warehouseId));
        conn.Execute("DELETE FROM new_order WHERE no_w_id = ?", MakeParams(warehouseId));
        conn.Execute("DELETE FROM oorder WHERE o_w_id = ?", MakeParams(warehouseId));
        conn.Execute("DELETE FROM history WHERE h_w_id = ? OR h_c_w_id = ?", MakeParams(warehouseId, warehouseId));
        conn.Execute("DELETE FROM customer WHERE c_w_id = ?", MakeParams(warehouseId));
        conn.Execute("DELETE FROM district WHERE d_w_id = ?", MakeParams(warehouseId));
        conn.Execute("DELETE FROM stock WHERE s_w_id = ?", MakeParams(warehouseId));
        conn.Execute("DELETE FROM warehouse WHERE w_id = ?", MakeParams(warehouseId));
        conn.Commit();
    } catch (...) {
        conn.Rollback();
        throw;
    }
}

void PopulateWarehouse(
    TObConnection& conn,
    uint64_t seed,
    int warehouseId,
    int batchRows)
{
    // Per-table commits (tpcc-oceanbase-cpp LoadWarehouse), not one warehouse-sized TX.
    InsertWarehouse(conn, seed, warehouseId, batchRows);
    InsertDistricts(conn, seed, warehouseId, batchRows);
    InsertStock(conn, seed, warehouseId, batchRows);
    for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
        InsertCustomers(conn, seed, warehouseId, d, batchRows);
        InsertHistory(conn, seed, warehouseId, d, batchRows);
        InsertOrders(conn, seed, warehouseId, d, batchRows);
    }
}

bool IsDuplicateKeyError(const std::exception& ex) {
    // MySQL / OceanBase ER_DUP_ENTRY
    if (const auto* db = dynamic_cast<const TObDbError*>(&ex)) {
        return db->Code() == 1062;
    }
    const std::string_view msg = ex.what();
    return msg.find("1062") != std::string_view::npos
        || msg.find("Duplicate entry") != std::string_view::npos;
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

        const std::string suffix =
            " ON DUPLICATE KEY UPDATE "
            "i_name = VALUES(i_name), i_price = VALUES(i_price), "
            "i_data = VALUES(i_data), i_im_id = VALUES(i_im_id)";
        BulkInsert(conn, "item",
            {"i_id", "i_name", "i_price", "i_data", "i_im_id"},
            batchRows,
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
        LOG_I("Items loaded (idempotent upsert)");
        return OkResult();
    } catch (const std::exception& ex) {
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
        LOG_D("Idempotent load warehouse " << warehouseId << " (seed=" << seed
              << ", run_id=" << (runId.empty() ? "-" : runId)
              << ", batch_rows=" << batchRows << ")");

        // Fast path: per-table BulkInsert commits (tpcc-oceanbase-cpp style).
        try {
            PopulateWarehouse(conn, seed, warehouseId, batchRows);
            return OkResult();
        } catch (const std::exception& ex) {
            if (!IsDuplicateKeyError(ex)) {
                throw;
            }
            LOG_I("Warehouse " << warehouseId
                  << " hit duplicate key (ERROR 1062); deleting range and reloading");
        }

        // Slow path: PK conflict means the range is occupied; wipe and reload.
        DeleteWarehouseChildren(conn, warehouseId);
        PopulateWarehouse(conn, seed, warehouseId, batchRows);
        return OkResult();
    } catch (const std::exception& ex) {
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

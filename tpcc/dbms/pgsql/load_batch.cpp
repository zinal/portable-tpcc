#include "load_batch.h"
#include "pqxx_compat.h"

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

namespace NTpcc {

namespace {

std::string FormatUnixTimestamp(int64_t unixSeconds) {
    time_t t = static_cast<time_t>(unixSeconds);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return ss.str();
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

void CopyWarehouse(pqxx::work& txn, uint64_t seed, int wh) {
    auto stream = MakeCopyStream(txn, "warehouse",
        {"w_id", "w_ytd", "w_tax", "w_name", "w_street_1", "w_street_2",
         "w_city", "w_state", "w_zip"});
    auto row = NGenerator::GenerateWarehouse(seed, wh);
    stream.write_values(
        row.Id,
        row.Ytd.ToString(),
        row.Tax.ToString(),
        row.Name,
        row.Street1,
        row.Street2,
        row.City,
        row.State,
        row.Zip);
    stream.complete();
}

void CopyDistricts(pqxx::work& txn, uint64_t seed, int wh) {
    auto stream = MakeCopyStream(txn, "district",
        {"d_w_id", "d_id", "d_ytd", "d_tax", "d_next_o_id", "d_name",
         "d_street_1", "d_street_2", "d_city", "d_state", "d_zip"});
    for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
        auto row = NGenerator::GenerateDistrict(seed, wh, d);
        stream.write_values(
            row.WarehouseId,
            row.Id,
            row.Ytd.ToString(),
            row.Tax.ToString(),
            row.NextOrderId,
            row.Name,
            row.Street1,
            row.Street2,
            row.City,
            row.State,
            row.Zip);
    }
    stream.complete();
}

void CopyStock(pqxx::work& txn, uint64_t seed, int wh, int batchRows) {
    const int chunk = batchRows > 0 ? batchRows : ITEM_COUNT;
    for (int start = 1; start <= ITEM_COUNT; start += chunk) {
        const int end = std::min(start + chunk - 1, ITEM_COUNT);
        auto stream = MakeCopyStream(txn, "stock",
            {"s_w_id", "s_i_id", "s_quantity", "s_ytd", "s_order_cnt", "s_remote_cnt",
             "s_data", "s_dist_01", "s_dist_02", "s_dist_03", "s_dist_04", "s_dist_05",
             "s_dist_06", "s_dist_07", "s_dist_08", "s_dist_09", "s_dist_10"});
        for (int itemId = start; itemId <= end; ++itemId) {
            auto row = NGenerator::GenerateStock(seed, wh, itemId);
            stream.write_values(
                row.WarehouseId,
                row.ItemId,
                row.Quantity,
                row.Ytd.ToString(),
                row.OrderCount,
                row.RemoteCount,
                row.Data,
                row.Dist[0],
                row.Dist[1],
                row.Dist[2],
                row.Dist[3],
                row.Dist[4],
                row.Dist[5],
                row.Dist[6],
                row.Dist[7],
                row.Dist[8],
                row.Dist[9]);
        }
        stream.complete();
    }
}

void CopyCustomers(pqxx::work& txn, uint64_t seed, int wh, int district, int batchRows) {
    const int chunk = batchRows > 0 ? batchRows : CUSTOMERS_PER_DISTRICT;
    for (int start = C_FIRST_CUSTOMER_ID; start <= CUSTOMERS_PER_DISTRICT; start += chunk) {
        const int end = std::min(start + chunk - 1, CUSTOMERS_PER_DISTRICT);
        auto stream = MakeCopyStream(txn, "customer",
            {"c_w_id", "c_d_id", "c_id", "c_discount", "c_credit", "c_last", "c_first",
             "c_credit_lim", "c_balance", "c_ytd_payment", "c_payment_cnt", "c_delivery_cnt",
             "c_street_1", "c_street_2", "c_city", "c_state", "c_zip", "c_phone",
             "c_since", "c_middle", "c_data"});
        for (int cid = start; cid <= end; ++cid) {
            auto row = NGenerator::GenerateCustomer(seed, wh, district, cid);
            stream.write_values(
                row.WarehouseId,
                row.DistrictId,
                row.Id,
                row.Discount.ToString(),
                row.Credit,
                row.Last,
                row.First,
                row.CreditLimit.ToString(),
                row.Balance.ToString(),
                row.YtdPayment.ToString(),
                row.PaymentCount,
                row.DeliveryCount,
                row.Street1,
                row.Street2,
                row.City,
                row.State,
                row.Zip,
                row.Phone,
                FormatUnixTimestamp(row.SinceUnix),
                row.Middle,
                row.Data);
        }
        stream.complete();
    }
}

void CopyHistory(pqxx::work& txn, uint64_t seed, int wh, int district) {
    auto stream = MakeCopyStream(txn, "history",
        {"h_c_id", "h_c_d_id", "h_c_w_id", "h_d_id", "h_w_id", "h_date", "h_amount", "h_data"});
    for (int cid = C_FIRST_CUSTOMER_ID; cid <= CUSTOMERS_PER_DISTRICT; ++cid) {
        auto row = NGenerator::GenerateHistory(seed, wh, district, cid);
        stream.write_values(
            row.CustomerId,
            row.CustomerDistrictId,
            row.CustomerWarehouseId,
            row.DistrictId,
            row.WarehouseId,
            FormatUnixTimestamp(row.DateUnix),
            row.Amount.ToString(),
            row.Data);
    }
    stream.complete();
}

void CopyOrders(pqxx::work& txn, uint64_t seed, int wh, int district) {
    const auto customerIds = NGenerator::InitialOrderCustomerPermutation(seed, wh, district);

    {
        auto stream = MakeCopyStream(txn, "oorder",
            {"o_w_id", "o_d_id", "o_id", "o_c_id", "o_carrier_id", "o_ol_cnt",
             "o_all_local", "o_entry_d"});
        for (int oid = 1; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
            int cid = customerIds[oid - 1];
            auto row = NGenerator::GenerateOrder(seed, wh, district, oid, cid);
            stream.write_values(
                row.WarehouseId,
                row.DistrictId,
                row.Id,
                row.CustomerId,
                row.CarrierId,
                row.OlCnt,
                row.AllLocal,
                FormatUnixTimestamp(row.EntryUnix));
        }
        stream.complete();
    }

    {
        auto stream = MakeCopyStream(txn, "new_order",
            {"no_w_id", "no_d_id", "no_o_id"});
        for (int oid = FIRST_UNPROCESSED_O_ID; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
            auto row = NGenerator::GenerateNewOrder(wh, district, oid);
            stream.write_values(row.WarehouseId, row.DistrictId, row.OrderId);
        }
        stream.complete();
    }

    {
        auto stream = MakeCopyStream(txn, "order_line",
            {"ol_w_id", "ol_d_id", "ol_o_id", "ol_number", "ol_i_id", "ol_delivery_d",
             "ol_amount", "ol_supply_w_id", "ol_quantity", "ol_dist_info"});
        for (int oid = 1; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
            int cid = customerIds[oid - 1];
            auto order = NGenerator::GenerateOrder(seed, wh, district, oid, cid);
            const bool delivered = oid < FIRST_UNPROCESSED_O_ID;
            // TPC-C §4.3.3.1: OL_DELIVERY_D must equal O_ENTRY_D for delivered orders.
            auto lines = NGenerator::GenerateOrderLines(
                seed, wh, district, oid, order.OlCnt,
                delivered ? std::optional<int64_t>(order.EntryUnix) : std::nullopt);
            for (const auto& line : lines) {
                std::optional<std::string> deliveryDate;
                if (line.DeliveryUnix) {
                    deliveryDate = FormatUnixTimestamp(*line.DeliveryUnix);
                }
                stream.write_values(
                    line.WarehouseId,
                    line.DistrictId,
                    line.OrderId,
                    line.Number,
                    line.ItemId,
                    deliveryDate,
                    line.Amount.ToString(),
                    line.SupplyWarehouseId,
                    line.Quantity,
                    line.DistInfo);
            }
        }
        stream.complete();
    }
}

} // namespace

TPutBatchResult PutItemsIdempotent(
    pqxx::connection& conn,
    uint64_t seed,
    const std::string& runId,
    int batchRows)
{
    try {
        LOG_I("Idempotent load of " << ITEM_COUNT << " items (seed=" << seed
              << ", run_id=" << (runId.empty() ? "-" : runId)
              << ", batch_rows=" << batchRows << ")");

        pqxx::work txn(conn);
        txn.exec(
            "CREATE TEMP TABLE item_stage ("
            "  i_id int NOT NULL,"
            "  i_name varchar(24) NOT NULL,"
            "  i_price decimal(5, 2) NOT NULL,"
            "  i_data varchar(50) NOT NULL,"
            "  i_im_id int NOT NULL"
            ") ON COMMIT DROP");

        {
            const int chunk = batchRows > 0 ? batchRows : ITEM_COUNT;
            for (int start = 1; start <= ITEM_COUNT; start += chunk) {
                const int end = std::min(start + chunk - 1, ITEM_COUNT);
                auto stream = MakeCopyStream(txn, "item_stage",
                    {"i_id", "i_name", "i_price", "i_data", "i_im_id"});
                for (int i = start; i <= end; ++i) {
                    auto row = NGenerator::GenerateItem(seed, i);
                    stream.write_values(
                        row.Id,
                        row.Name,
                        row.Price.ToString(),
                        row.Data,
                        row.ImageId);
                }
                stream.complete();
            }
        }

        txn.exec(
            "INSERT INTO item AS i (i_id, i_name, i_price, i_data, i_im_id) "
            "SELECT i_id, i_name, i_price, i_data, i_im_id FROM item_stage "
            "ON CONFLICT (i_id) DO UPDATE SET "
            "  i_name = EXCLUDED.i_name, "
            "  i_price = EXCLUDED.i_price, "
            "  i_data = EXCLUDED.i_data, "
            "  i_im_id = EXCLUDED.i_im_id");

        txn.commit();
        LOG_I("Items loaded (idempotent upsert)");
        return OkResult();
    } catch (const std::exception& ex) {
        LOG_E("PutItemsIdempotent failed: " << ex.what());
        return FailResult(ex);
    }
}

TPutBatchResult PutWarehouseIdempotent(
    pqxx::connection& conn,
    uint64_t seed,
    int warehouseId,
    [[maybe_unused]] const std::string& runId,
    int batchRows)
{
    try {
        LOG_D("Idempotent replace warehouse " << warehouseId << " (seed=" << seed
              << ", run_id=" << (runId.empty() ? "-" : runId)
              << ", batch_rows=" << batchRows << ")");

        pqxx::work txn(conn);

        // ON DELETE CASCADE clears district/stock/customer/history/orders for this WH.
        txn.exec_params("DELETE FROM warehouse WHERE w_id = $1", warehouseId);

        CopyWarehouse(txn, seed, warehouseId);
        CopyDistricts(txn, seed, warehouseId);
        CopyStock(txn, seed, warehouseId, batchRows);
        for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
            CopyCustomers(txn, seed, warehouseId, d, batchRows);
            CopyHistory(txn, seed, warehouseId, d);
            CopyOrders(txn, seed, warehouseId, d);
        }

        txn.commit();
        return OkResult();
    } catch (const std::exception& ex) {
        LOG_E("PutWarehouseIdempotent(w_id=" << warehouseId << ") failed: " << ex.what());
        return FailResult(ex);
    }
}

TPgLoadAdapter::TPgLoadAdapter(pqxx::connection& conn, uint64_t seed)
    : Conn_(conn)
    , Seed_(seed)
{}

TPutBatchResult TPgLoadAdapter::PutBatch(
    const std::string& runId,
    const TLoadKeyRange& keyRange,
    const std::vector<std::string>& rows)
{
    if (!rows.empty()) {
        TPutBatchResult r;
        r.Outcome = EPutBatchOutcome::Failed;
        r.Message = "TPgLoadAdapter does not yet accept pre-serialized rows; "
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
            auto result = PutWarehouseIdempotent(
                Conn_, Seed_, static_cast<int>(wh), runId);
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

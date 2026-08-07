#include "load_batch.h"

#include <constants.h>
#include <log.h>
#include <populate.h>

#include <fmt/format.h>
#include <util/datetime/base.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <algorithm>
#include <stdexcept>

namespace NTpcc {

namespace {

using NYdb::TDecimalValue;
using NYdb::TValue;
using NYdb::TValueBuilder;

constexpr uint8_t MONEY_PRECISION = 22;
constexpr uint8_t MONEY_SCALE = 9;

TDecimalValue Decimal(TMoney value) {
    return TDecimalValue(value.ToString(), MONEY_PRECISION, MONEY_SCALE);
}

TDecimalValue Decimal(TRate value) {
    return TDecimalValue(value.ToString(), MONEY_PRECISION, MONEY_SCALE);
}

TInstant Instant(int64_t unixSeconds) {
    return TInstant::Seconds(unixSeconds);
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

void ThrowIfFailed(const NYdb::TStatus& status, const std::string& what) {
    if (!status.IsSuccess()) {
        throw std::runtime_error(what + ": " + status.GetIssues().ToOneLineString());
    }
}

void BulkUpsert(TYdbConnection& connection, const std::string& table, TValue rows) {
    auto status = connection.TableClient().BulkUpsert(
        connection.TablePath(table),
        std::move(rows)).GetValueSync();
    ThrowIfFailed(status, "bulk upsert " + table);
}

void DeleteWarehouseRows(TYdbConnection& connection, int wh) {
    auto params = NYdb::TParamsBuilder()
        .AddParam("$w_id").Int32(wh).Build()
        .Build();
    // YQL without TablePathPrefix: use database-relative table paths.
    const std::string query = fmt::format(R"(
        DECLARE $w_id AS Int32;
        DELETE FROM `{0}` WHERE ol_w_id = $w_id;
        DELETE FROM `{1}` WHERE no_w_id = $w_id;
        DELETE FROM `{2}` WHERE o_w_id = $w_id;
        DELETE FROM `{3}` WHERE h_c_w_id = $w_id;
        DELETE FROM `{4}` WHERE c_w_id = $w_id;
        DELETE FROM `{5}` WHERE d_w_id = $w_id;
        DELETE FROM `{6}` WHERE s_w_id = $w_id;
        DELETE FROM `{7}` WHERE w_id = $w_id;
    )",
        connection.RelativeTablePath(TABLE_ORDER_LINE),
        connection.RelativeTablePath(TABLE_NEW_ORDER),
        connection.RelativeTablePath(TABLE_OORDER),
        connection.RelativeTablePath(TABLE_HISTORY),
        connection.RelativeTablePath(TABLE_CUSTOMER),
        connection.RelativeTablePath(TABLE_DISTRICT),
        connection.RelativeTablePath(TABLE_STOCK),
        connection.RelativeTablePath(TABLE_WAREHOUSE));
    auto status = connection.QueryClient().RetryQuerySync([&](NYdb::NQuery::TSession session) {
        return session.ExecuteQuery(
            query,
            NYdb::NQuery::TTxControl::BeginTx(NYdb::NQuery::TTxSettings::SerializableRW()).CommitTx(),
            params).GetValueSync();
    });
    ThrowIfFailed(status, "delete warehouse rows");
}

} // anonymous

TPutBatchResult PutItemsIdempotent(
    TYdbConnection& connection,
    uint64_t seed,
    const std::string& runId,
    int batchRows)
{
    try {
        LOG_I("YDB idempotent load of " << ITEM_COUNT << " items (seed=" << seed
              << ", run_id=" << (runId.empty() ? "-" : runId)
              << ", batch_rows=" << batchRows << ")");
        const int chunk = batchRows > 0 ? batchRows : ITEM_COUNT;
        for (int start = 1; start <= ITEM_COUNT; start += chunk) {
            const int end = std::min(start + chunk - 1, ITEM_COUNT);
            TValueBuilder rows;
            rows.BeginList();
            for (int id = start; id <= end; ++id) {
                auto row = NGenerator::GenerateItem(seed, id);
                rows.AddListItem()
                    .BeginStruct()
                    .AddMember("i_id").Int32(row.Id)
                    .AddMember("i_name").Utf8(row.Name)
                    .AddMember("i_price").Decimal(Decimal(row.Price))
                    .AddMember("i_data").Utf8(row.Data)
                    .AddMember("i_im_id").Int32(row.ImageId)
                    .EndStruct();
            }
            rows.EndList();
            BulkUpsert(connection, TABLE_ITEM, rows.Build());
        }
        return OkResult();
    } catch (const std::exception& ex) {
        return FailResult(ex);
    }
}

TPutBatchResult PutWarehouseIdempotent(
    TYdbConnection& connection,
    uint64_t seed,
    int warehouseId,
    const std::string& runId,
    int batchRows)
{
    try {
        LOG_I("YDB idempotent load of warehouse " << warehouseId << " (seed=" << seed
              << ", run_id=" << (runId.empty() ? "-" : runId)
              << ", batch_rows=" << batchRows << ")");

        DeleteWarehouseRows(connection, warehouseId);

        {
            TValueBuilder rows;
            rows.BeginList();
            auto row = NGenerator::GenerateWarehouse(seed, warehouseId);
            rows.AddListItem()
                .BeginStruct()
                .AddMember("w_id").Int32(row.Id)
                .AddMember("w_ytd").Decimal(Decimal(row.Ytd))
                .AddMember("w_tax").Decimal(Decimal(row.Tax))
                .AddMember("w_name").Utf8(row.Name)
                .AddMember("w_street_1").Utf8(row.Street1)
                .AddMember("w_street_2").Utf8(row.Street2)
                .AddMember("w_city").Utf8(row.City)
                .AddMember("w_state").Utf8(row.State)
                .AddMember("w_zip").Utf8(row.Zip)
                .EndStruct();
            rows.EndList();
            BulkUpsert(connection, TABLE_WAREHOUSE, rows.Build());
        }

        {
            TValueBuilder rows;
            rows.BeginList();
            for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
                auto row = NGenerator::GenerateDistrict(seed, warehouseId, d);
                rows.AddListItem()
                    .BeginStruct()
                    .AddMember("d_w_id").Int32(row.WarehouseId)
                    .AddMember("d_id").Int32(row.Id)
                    .AddMember("d_ytd").Decimal(Decimal(row.Ytd))
                    .AddMember("d_tax").Decimal(Decimal(row.Tax))
                    .AddMember("d_next_o_id").Int32(row.NextOrderId)
                    .AddMember("d_name").Utf8(row.Name)
                    .AddMember("d_street_1").Utf8(row.Street1)
                    .AddMember("d_street_2").Utf8(row.Street2)
                    .AddMember("d_city").Utf8(row.City)
                    .AddMember("d_state").Utf8(row.State)
                    .AddMember("d_zip").Utf8(row.Zip)
                    .EndStruct();
            }
            rows.EndList();
            BulkUpsert(connection, TABLE_DISTRICT, rows.Build());
        }

        const int stockChunk = batchRows > 0 ? batchRows : ITEM_COUNT;
        for (int start = 1; start <= ITEM_COUNT; start += stockChunk) {
            const int end = std::min(start + stockChunk - 1, ITEM_COUNT);
            TValueBuilder rows;
            rows.BeginList();
            for (int itemId = start; itemId <= end; ++itemId) {
                auto row = NGenerator::GenerateStock(seed, warehouseId, itemId);
                rows.AddListItem()
                    .BeginStruct()
                    .AddMember("s_w_id").Int32(row.WarehouseId)
                    .AddMember("s_i_id").Int32(row.ItemId)
                    .AddMember("s_quantity").Int32(row.Quantity)
                    .AddMember("s_ytd").Decimal(Decimal(row.Ytd))
                    .AddMember("s_order_cnt").Int32(row.OrderCount)
                    .AddMember("s_remote_cnt").Int32(row.RemoteCount)
                    .AddMember("s_data").Utf8(row.Data)
                    .AddMember("s_dist_01").Utf8(row.Dist[0])
                    .AddMember("s_dist_02").Utf8(row.Dist[1])
                    .AddMember("s_dist_03").Utf8(row.Dist[2])
                    .AddMember("s_dist_04").Utf8(row.Dist[3])
                    .AddMember("s_dist_05").Utf8(row.Dist[4])
                    .AddMember("s_dist_06").Utf8(row.Dist[5])
                    .AddMember("s_dist_07").Utf8(row.Dist[6])
                    .AddMember("s_dist_08").Utf8(row.Dist[7])
                    .AddMember("s_dist_09").Utf8(row.Dist[8])
                    .AddMember("s_dist_10").Utf8(row.Dist[9])
                    .EndStruct();
            }
            rows.EndList();
            BulkUpsert(connection, TABLE_STOCK, rows.Build());
        }

        for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
            const int customerChunk = batchRows > 0 ? batchRows : CUSTOMERS_PER_DISTRICT;
            for (int start = C_FIRST_CUSTOMER_ID; start <= CUSTOMERS_PER_DISTRICT; start += customerChunk) {
                const int end = std::min(start + customerChunk - 1, CUSTOMERS_PER_DISTRICT);
                TValueBuilder customers;
                customers.BeginList();
                TValueBuilder history;
                history.BeginList();
                for (int cid = start; cid <= end; ++cid) {
                    auto c = NGenerator::GenerateCustomer(seed, warehouseId, d, cid);
                    customers.AddListItem()
                        .BeginStruct()
                        .AddMember("c_w_id").Int32(c.WarehouseId)
                        .AddMember("c_d_id").Int32(c.DistrictId)
                        .AddMember("c_id").Int32(c.Id)
                        .AddMember("c_discount").Decimal(Decimal(c.Discount))
                        .AddMember("c_credit").Utf8(c.Credit)
                        .AddMember("c_last").Utf8(c.Last)
                        .AddMember("c_first").Utf8(c.First)
                        .AddMember("c_credit_lim").Decimal(Decimal(c.CreditLimit))
                        .AddMember("c_balance").Decimal(Decimal(c.Balance))
                        .AddMember("c_ytd_payment").Decimal(Decimal(c.YtdPayment))
                        .AddMember("c_payment_cnt").Int32(c.PaymentCount)
                        .AddMember("c_delivery_cnt").Int32(c.DeliveryCount)
                        .AddMember("c_street_1").Utf8(c.Street1)
                        .AddMember("c_street_2").Utf8(c.Street2)
                        .AddMember("c_city").Utf8(c.City)
                        .AddMember("c_state").Utf8(c.State)
                        .AddMember("c_zip").Utf8(c.Zip)
                        .AddMember("c_phone").Utf8(c.Phone)
                        .AddMember("c_since").Timestamp(Instant(c.SinceUnix))
                        .AddMember("c_middle").Utf8(c.Middle)
                        .AddMember("c_data").Utf8(c.Data)
                        .EndStruct();

                    auto h = NGenerator::GenerateHistory(seed, warehouseId, d, cid);
                    const int64_t nanoTs = h.DateUnix * 1000000000LL + cid;
                    history.AddListItem()
                        .BeginStruct()
                        .AddMember("h_c_w_id").Int32(h.CustomerWarehouseId)
                        .AddMember("h_c_d_id").Int32(h.CustomerDistrictId)
                        .AddMember("h_c_id").Int32(h.CustomerId)
                        .AddMember("h_c_nano_ts").Int64(nanoTs)
                        .AddMember("h_d_id").Int32(h.DistrictId)
                        .AddMember("h_w_id").Int32(h.WarehouseId)
                        .AddMember("h_date").Timestamp(Instant(h.DateUnix))
                        .AddMember("h_amount").Decimal(Decimal(h.Amount))
                        .AddMember("h_data").Utf8(h.Data)
                        .EndStruct();
                }
                customers.EndList();
                history.EndList();
                BulkUpsert(connection, TABLE_CUSTOMER, customers.Build());
                BulkUpsert(connection, TABLE_HISTORY, history.Build());
            }

            const auto customerIds = NGenerator::InitialOrderCustomerPermutation(seed, warehouseId, d);
            TValueBuilder orders;
            orders.BeginList();
            TValueBuilder newOrders;
            newOrders.BeginList();
            TValueBuilder orderLines;
            orderLines.BeginList();
            for (int oid = 1; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
                const int cid = customerIds[oid - 1];
                auto order = NGenerator::GenerateOrder(seed, warehouseId, d, oid, cid);
                orders.AddListItem()
                    .BeginStruct()
                    .AddMember("o_w_id").Int32(order.WarehouseId)
                    .AddMember("o_d_id").Int32(order.DistrictId)
                    .AddMember("o_id").Int32(order.Id)
                    .AddMember("o_c_id").Int32(order.CustomerId)
                    .AddMember("o_carrier_id").OptionalInt32(order.CarrierId)
                    .AddMember("o_ol_cnt").Int32(order.OlCnt)
                    .AddMember("o_all_local").Int32(order.AllLocal)
                    .AddMember("o_entry_d").Timestamp(Instant(order.EntryUnix))
                    .EndStruct();

                if (oid >= FIRST_UNPROCESSED_O_ID) {
                    auto no = NGenerator::GenerateNewOrder(warehouseId, d, oid);
                    newOrders.AddListItem()
                        .BeginStruct()
                        .AddMember("no_w_id").Int32(no.WarehouseId)
                        .AddMember("no_d_id").Int32(no.DistrictId)
                        .AddMember("no_o_id").Int32(no.OrderId)
                        .EndStruct();
                }

                const bool delivered = oid < FIRST_UNPROCESSED_O_ID;
                auto lines = NGenerator::GenerateOrderLines(
                    seed, warehouseId, d, oid, order.OlCnt,
                    delivered ? std::optional<int64_t>(order.EntryUnix) : std::nullopt);
                for (const auto& line : lines) {
                    std::optional<TInstant> deliveryTs;
                    if (line.DeliveryUnix) {
                        deliveryTs = Instant(*line.DeliveryUnix);
                    }
                    orderLines.AddListItem()
                        .BeginStruct()
                        .AddMember("ol_w_id").Int32(line.WarehouseId)
                        .AddMember("ol_d_id").Int32(line.DistrictId)
                        .AddMember("ol_o_id").Int32(line.OrderId)
                        .AddMember("ol_number").Int32(line.Number)
                        .AddMember("ol_i_id").Int32(line.ItemId)
                        .AddMember("ol_delivery_d").OptionalTimestamp(deliveryTs)
                        .AddMember("ol_amount").Decimal(Decimal(line.Amount))
                        .AddMember("ol_supply_w_id").Int32(line.SupplyWarehouseId)
                        .AddMember("ol_quantity").Int32(line.Quantity)
                        .AddMember("ol_dist_info").Utf8(line.DistInfo)
                        .EndStruct();
                }
            }
            orders.EndList();
            newOrders.EndList();
            orderLines.EndList();
            BulkUpsert(connection, TABLE_OORDER, orders.Build());
            BulkUpsert(connection, TABLE_NEW_ORDER, newOrders.Build());
            BulkUpsert(connection, TABLE_ORDER_LINE, orderLines.Build());
        }

        return OkResult();
    } catch (const std::exception& ex) {
        return FailResult(ex);
    }
}

TYdbLoadAdapter::TYdbLoadAdapter(TYdbConnection& connection, uint64_t seed)
    : Connection_(connection)
    , Seed_(seed)
{}

TPutBatchResult TYdbLoadAdapter::PutBatch(
    const std::string& runId,
    const TLoadKeyRange& keyRange,
    const std::vector<std::string>& rows)
{
    if (!rows.empty()) {
        TPutBatchResult r;
        r.Outcome = EPutBatchOutcome::Failed;
        r.Message = "YDB load adapter regenerates deterministic rows; serialized rows are not supported";
        return r;
    }
    if (keyRange.Table == TABLE_ITEM) {
        return PutItemsIdempotent(Connection_, Seed_, runId, 0);
    }
    if (keyRange.Table == TABLE_WAREHOUSE) {
        for (int64_t wh = keyRange.Begin; wh < keyRange.End; ++wh) {
            auto result = PutWarehouseIdempotent(Connection_, Seed_, static_cast<int>(wh), runId, 0);
            if (result.Outcome != EPutBatchOutcome::Completed) {
                return result;
            }
        }
        return OkResult();
    }
    TPutBatchResult r;
    r.Outcome = EPutBatchOutcome::Failed;
    r.Message = "unsupported YDB load table: " + keyRange.Table;
    return r;
}

} // namespace NTpcc

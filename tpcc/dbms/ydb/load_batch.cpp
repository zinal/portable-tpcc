#include "load_batch.h"

#include "arrow_upsert.h"

#include <constants.h>
#include <log.h>
#include <populate.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/api.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace NTpcc {

namespace {

using NYdb::TDecimalValue;

constexpr uint8_t MONEY_PRECISION = 22;
constexpr uint8_t MONEY_SCALE = 9;
constexpr int DECIMAL_BINARY_WIDTH = 16;

TDecimalValue ToDecimal(TMoney value) {
    return TDecimalValue(value.ToString(), MONEY_PRECISION, MONEY_SCALE);
}

TDecimalValue ToDecimal(TRate value) {
    return TDecimalValue(value.ToString(), MONEY_PRECISION, MONEY_SCALE);
}

int64_t ToTimestampMicros(int64_t unixSeconds) {
    return unixSeconds * 1'000'000LL;
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

void ThrowArrow(const arrow::Status& status, const char* what) {
    if (!status.ok()) {
        throw std::runtime_error(std::string(what) + ": " + status.ToString());
    }
}

std::shared_ptr<arrow::DataType> DecimalBinaryType() {
    return arrow::fixed_size_binary(DECIMAL_BINARY_WIDTH);
}

std::shared_ptr<arrow::DataType> TimestampType() {
    return arrow::timestamp(arrow::TimeUnit::MICRO);
}

void AppendDecimal(arrow::FixedSizeBinaryBuilder& builder, const TDecimalValue& value) {
    alignas(16) char buf[DECIMAL_BINARY_WIDTH];
    std::memcpy(buf, &value.Low_, sizeof(value.Low_));
    std::memcpy(buf + sizeof(value.Low_), &value.Hi_, sizeof(value.Hi_));
    ThrowArrow(builder.Append(buf), "append decimal");
}

std::shared_ptr<arrow::Array> Finish(arrow::ArrayBuilder& builder) {
    std::shared_ptr<arrow::Array> array;
    ThrowArrow(builder.Finish(&array), "finish Arrow array");
    return array;
}

std::shared_ptr<arrow::RecordBatch> MakeBatch(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::vector<std::shared_ptr<arrow::Array>>& columns,
    int64_t rows)
{
    return arrow::RecordBatch::Make(schema, rows, columns);
}

const std::shared_ptr<arrow::Schema>& ItemSchema() {
    static const auto schema = arrow::schema({
        arrow::field("i_id", arrow::int32(), false),
        arrow::field("i_name", arrow::utf8(), false),
        arrow::field("i_price", DecimalBinaryType(), false),
        arrow::field("i_data", arrow::utf8(), false),
        arrow::field("i_im_id", arrow::int32(), false),
    });
    return schema;
}

const std::shared_ptr<arrow::Schema>& WarehouseSchema() {
    static const auto schema = arrow::schema({
        arrow::field("w_id", arrow::int32(), false),
        arrow::field("w_ytd", DecimalBinaryType(), false),
        arrow::field("w_tax", DecimalBinaryType(), false),
        arrow::field("w_name", arrow::utf8(), false),
        arrow::field("w_street_1", arrow::utf8(), false),
        arrow::field("w_street_2", arrow::utf8(), false),
        arrow::field("w_city", arrow::utf8(), false),
        arrow::field("w_state", arrow::utf8(), false),
        arrow::field("w_zip", arrow::utf8(), false),
    });
    return schema;
}

const std::shared_ptr<arrow::Schema>& DistrictSchema() {
    static const auto schema = arrow::schema({
        arrow::field("d_w_id", arrow::int32(), false),
        arrow::field("d_id", arrow::int32(), false),
        arrow::field("d_ytd", DecimalBinaryType(), false),
        arrow::field("d_tax", DecimalBinaryType(), false),
        arrow::field("d_next_o_id", arrow::int32(), false),
        arrow::field("d_name", arrow::utf8(), false),
        arrow::field("d_street_1", arrow::utf8(), false),
        arrow::field("d_street_2", arrow::utf8(), false),
        arrow::field("d_city", arrow::utf8(), false),
        arrow::field("d_state", arrow::utf8(), false),
        arrow::field("d_zip", arrow::utf8(), false),
    });
    return schema;
}

const std::shared_ptr<arrow::Schema>& StockSchema() {
    static const auto schema = arrow::schema({
        arrow::field("s_w_id", arrow::int32(), false),
        arrow::field("s_i_id", arrow::int32(), false),
        arrow::field("s_quantity", arrow::int32(), false),
        arrow::field("s_ytd", DecimalBinaryType(), false),
        arrow::field("s_order_cnt", arrow::int32(), false),
        arrow::field("s_remote_cnt", arrow::int32(), false),
        arrow::field("s_data", arrow::utf8(), false),
        arrow::field("s_dist_01", arrow::utf8(), false),
        arrow::field("s_dist_02", arrow::utf8(), false),
        arrow::field("s_dist_03", arrow::utf8(), false),
        arrow::field("s_dist_04", arrow::utf8(), false),
        arrow::field("s_dist_05", arrow::utf8(), false),
        arrow::field("s_dist_06", arrow::utf8(), false),
        arrow::field("s_dist_07", arrow::utf8(), false),
        arrow::field("s_dist_08", arrow::utf8(), false),
        arrow::field("s_dist_09", arrow::utf8(), false),
        arrow::field("s_dist_10", arrow::utf8(), false),
    });
    return schema;
}

const std::shared_ptr<arrow::Schema>& CustomerSchema() {
    static const auto schema = arrow::schema({
        arrow::field("c_w_id", arrow::int32(), false),
        arrow::field("c_d_id", arrow::int32(), false),
        arrow::field("c_id", arrow::int32(), false),
        arrow::field("c_discount", DecimalBinaryType(), false),
        arrow::field("c_credit", arrow::utf8(), false),
        arrow::field("c_last", arrow::utf8(), false),
        arrow::field("c_first", arrow::utf8(), false),
        arrow::field("c_credit_lim", DecimalBinaryType(), false),
        arrow::field("c_balance", DecimalBinaryType(), false),
        arrow::field("c_ytd_payment", DecimalBinaryType(), false),
        arrow::field("c_payment_cnt", arrow::int32(), false),
        arrow::field("c_delivery_cnt", arrow::int32(), false),
        arrow::field("c_street_1", arrow::utf8(), false),
        arrow::field("c_street_2", arrow::utf8(), false),
        arrow::field("c_city", arrow::utf8(), false),
        arrow::field("c_state", arrow::utf8(), false),
        arrow::field("c_zip", arrow::utf8(), false),
        arrow::field("c_phone", arrow::utf8(), false),
        arrow::field("c_since", TimestampType(), false),
        arrow::field("c_middle", arrow::utf8(), false),
        arrow::field("c_data", arrow::utf8(), false),
    });
    return schema;
}

const std::shared_ptr<arrow::Schema>& HistorySchema() {
    static const auto schema = arrow::schema({
        arrow::field("h_c_w_id", arrow::int32(), false),
        arrow::field("h_c_d_id", arrow::int32(), false),
        arrow::field("h_c_id", arrow::int32(), false),
        arrow::field("h_c_nano_ts", arrow::int64(), false),
        arrow::field("h_d_id", arrow::int32(), false),
        arrow::field("h_w_id", arrow::int32(), false),
        arrow::field("h_date", TimestampType(), false),
        arrow::field("h_amount", DecimalBinaryType(), false),
        arrow::field("h_data", arrow::utf8(), false),
    });
    return schema;
}

const std::shared_ptr<arrow::Schema>& OorderSchema() {
    static const auto schema = arrow::schema({
        arrow::field("o_w_id", arrow::int32(), false),
        arrow::field("o_d_id", arrow::int32(), false),
        arrow::field("o_id", arrow::int32(), false),
        arrow::field("o_c_id", arrow::int32(), false),
        arrow::field("o_carrier_id", arrow::int32(), true),
        arrow::field("o_ol_cnt", arrow::int32(), false),
        arrow::field("o_all_local", arrow::int32(), false),
        arrow::field("o_entry_d", TimestampType(), false),
    });
    return schema;
}

const std::shared_ptr<arrow::Schema>& NewOrderSchema() {
    static const auto schema = arrow::schema({
        arrow::field("no_w_id", arrow::int32(), false),
        arrow::field("no_d_id", arrow::int32(), false),
        arrow::field("no_o_id", arrow::int32(), false),
    });
    return schema;
}

const std::shared_ptr<arrow::Schema>& OrderLineSchema() {
    static const auto schema = arrow::schema({
        arrow::field("ol_w_id", arrow::int32(), false),
        arrow::field("ol_d_id", arrow::int32(), false),
        arrow::field("ol_o_id", arrow::int32(), false),
        arrow::field("ol_number", arrow::int32(), false),
        arrow::field("ol_i_id", arrow::int32(), false),
        arrow::field("ol_delivery_d", TimestampType(), true),
        arrow::field("ol_amount", DecimalBinaryType(), false),
        arrow::field("ol_supply_w_id", arrow::int32(), false),
        arrow::field("ol_quantity", arrow::int32(), false),
        arrow::field("ol_dist_info", arrow::utf8(), false),
    });
    return schema;
}

} // anonymous

TPutBatchResult PutItemsIdempotent(
    TYdbConnection& connection,
    uint64_t seed,
    const std::string& runId,
    int batchRows)
{
    try {
        LOG_I("YDB Arrow load of " << ITEM_COUNT << " items (seed=" << seed
              << ", run_id=" << (runId.empty() ? "-" : runId)
              << ", batch_rows=" << batchRows << ")");
        const int chunk = batchRows > 0 ? batchRows : ITEM_COUNT;
        for (int start = 1; start <= ITEM_COUNT; start += chunk) {
            const int end = std::min(start + chunk - 1, ITEM_COUNT);
            const int64_t rows = end - start + 1;

            arrow::Int32Builder id;
            arrow::StringBuilder name;
            arrow::FixedSizeBinaryBuilder price(DecimalBinaryType());
            arrow::StringBuilder data;
            arrow::Int32Builder imageId;
            ThrowArrow(id.Reserve(rows), "reserve i_id");
            ThrowArrow(name.Reserve(rows), "reserve i_name");
            ThrowArrow(price.Reserve(rows), "reserve i_price");
            ThrowArrow(data.Reserve(rows), "reserve i_data");
            ThrowArrow(imageId.Reserve(rows), "reserve i_im_id");

            for (int itemId = start; itemId <= end; ++itemId) {
                auto row = NGenerator::GenerateItem(seed, itemId);
                ThrowArrow(id.Append(row.Id), "append i_id");
                ThrowArrow(name.Append(row.Name), "append i_name");
                AppendDecimal(price, ToDecimal(row.Price));
                ThrowArrow(data.Append(row.Data), "append i_data");
                ThrowArrow(imageId.Append(row.ImageId), "append i_im_id");
            }

            auto batch = MakeBatch(ItemSchema(), {
                Finish(id), Finish(name), Finish(price), Finish(data), Finish(imageId),
            }, rows);
            BulkUpsertArrow(connection, TABLE_ITEM, batch);
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
        LOG_I("YDB Arrow load of warehouse " << warehouseId << " (seed=" << seed
              << ", run_id=" << (runId.empty() ? "-" : runId)
              << ", batch_rows=" << batchRows << ")");

        {
            auto row = NGenerator::GenerateWarehouse(seed, warehouseId);
            arrow::Int32Builder id;
            arrow::FixedSizeBinaryBuilder ytd(DecimalBinaryType());
            arrow::FixedSizeBinaryBuilder tax(DecimalBinaryType());
            arrow::StringBuilder name, street1, street2, city, state, zip;
            ThrowArrow(id.Append(row.Id), "append w_id");
            AppendDecimal(ytd, ToDecimal(row.Ytd));
            AppendDecimal(tax, ToDecimal(row.Tax));
            ThrowArrow(name.Append(row.Name), "append w_name");
            ThrowArrow(street1.Append(row.Street1), "append w_street_1");
            ThrowArrow(street2.Append(row.Street2), "append w_street_2");
            ThrowArrow(city.Append(row.City), "append w_city");
            ThrowArrow(state.Append(row.State), "append w_state");
            ThrowArrow(zip.Append(row.Zip), "append w_zip");
            BulkUpsertArrow(connection, TABLE_WAREHOUSE, MakeBatch(WarehouseSchema(), {
                Finish(id), Finish(ytd), Finish(tax), Finish(name),
                Finish(street1), Finish(street2), Finish(city), Finish(state), Finish(zip),
            }, 1));
        }

        {
            constexpr int64_t rows = DISTRICT_COUNT;
            arrow::Int32Builder wId, dId, nextOId;
            arrow::FixedSizeBinaryBuilder ytd(DecimalBinaryType());
            arrow::FixedSizeBinaryBuilder tax(DecimalBinaryType());
            arrow::StringBuilder name, street1, street2, city, state, zip;
            ThrowArrow(wId.Reserve(rows), "reserve d_w_id");
            for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
                auto row = NGenerator::GenerateDistrict(seed, warehouseId, d);
                ThrowArrow(wId.Append(row.WarehouseId), "append d_w_id");
                ThrowArrow(dId.Append(row.Id), "append d_id");
                AppendDecimal(ytd, ToDecimal(row.Ytd));
                AppendDecimal(tax, ToDecimal(row.Tax));
                ThrowArrow(nextOId.Append(row.NextOrderId), "append d_next_o_id");
                ThrowArrow(name.Append(row.Name), "append d_name");
                ThrowArrow(street1.Append(row.Street1), "append d_street_1");
                ThrowArrow(street2.Append(row.Street2), "append d_street_2");
                ThrowArrow(city.Append(row.City), "append d_city");
                ThrowArrow(state.Append(row.State), "append d_state");
                ThrowArrow(zip.Append(row.Zip), "append d_zip");
            }
            BulkUpsertArrow(connection, TABLE_DISTRICT, MakeBatch(DistrictSchema(), {
                Finish(wId), Finish(dId), Finish(ytd), Finish(tax), Finish(nextOId),
                Finish(name), Finish(street1), Finish(street2), Finish(city), Finish(state), Finish(zip),
            }, rows));
        }

        const int stockChunk = batchRows > 0 ? batchRows : ITEM_COUNT;
        for (int start = 1; start <= ITEM_COUNT; start += stockChunk) {
            const int end = std::min(start + stockChunk - 1, ITEM_COUNT);
            const int64_t rows = end - start + 1;
            arrow::Int32Builder wId, iId, quantity, orderCnt, remoteCnt;
            arrow::FixedSizeBinaryBuilder ytd(DecimalBinaryType());
            arrow::StringBuilder data;
            std::array<arrow::StringBuilder, 10> dist;
            ThrowArrow(wId.Reserve(rows), "reserve s_w_id");
            for (int itemId = start; itemId <= end; ++itemId) {
                auto row = NGenerator::GenerateStock(seed, warehouseId, itemId);
                ThrowArrow(wId.Append(row.WarehouseId), "append s_w_id");
                ThrowArrow(iId.Append(row.ItemId), "append s_i_id");
                ThrowArrow(quantity.Append(row.Quantity), "append s_quantity");
                AppendDecimal(ytd, ToDecimal(row.Ytd));
                ThrowArrow(orderCnt.Append(row.OrderCount), "append s_order_cnt");
                ThrowArrow(remoteCnt.Append(row.RemoteCount), "append s_remote_cnt");
                ThrowArrow(data.Append(row.Data), "append s_data");
                for (int i = 0; i < 10; ++i) {
                    ThrowArrow(dist[i].Append(row.Dist[i]), "append s_dist");
                }
            }
            BulkUpsertArrow(connection, TABLE_STOCK, MakeBatch(StockSchema(), {
                Finish(wId), Finish(iId), Finish(quantity), Finish(ytd),
                Finish(orderCnt), Finish(remoteCnt), Finish(data),
                Finish(dist[0]), Finish(dist[1]), Finish(dist[2]), Finish(dist[3]), Finish(dist[4]),
                Finish(dist[5]), Finish(dist[6]), Finish(dist[7]), Finish(dist[8]), Finish(dist[9]),
            }, rows));
        }

        for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
            const int customerChunk = batchRows > 0 ? batchRows : CUSTOMERS_PER_DISTRICT;
            for (int start = C_FIRST_CUSTOMER_ID; start <= CUSTOMERS_PER_DISTRICT; start += customerChunk) {
                const int end = std::min(start + customerChunk - 1, CUSTOMERS_PER_DISTRICT);
                const int64_t rows = end - start + 1;

                arrow::Int32Builder cWId, cDId, cId, paymentCnt, deliveryCnt;
                arrow::FixedSizeBinaryBuilder discount(DecimalBinaryType());
                arrow::FixedSizeBinaryBuilder creditLim(DecimalBinaryType());
                arrow::FixedSizeBinaryBuilder balance(DecimalBinaryType());
                arrow::FixedSizeBinaryBuilder ytdPayment(DecimalBinaryType());
                arrow::StringBuilder credit, last, first, street1, street2, city, state, zip, phone, middle, data;
                arrow::TimestampBuilder since(TimestampType(), arrow::default_memory_pool());

                arrow::Int32Builder hCWId, hCDId, hCId, hDId, hWId;
                arrow::Int64Builder hNanoTs;
                arrow::TimestampBuilder hDate(TimestampType(), arrow::default_memory_pool());
                arrow::FixedSizeBinaryBuilder hAmount(DecimalBinaryType());
                arrow::StringBuilder hData;

                for (int cid = start; cid <= end; ++cid) {
                    auto c = NGenerator::GenerateCustomer(seed, warehouseId, d, cid);
                    ThrowArrow(cWId.Append(c.WarehouseId), "append c_w_id");
                    ThrowArrow(cDId.Append(c.DistrictId), "append c_d_id");
                    ThrowArrow(cId.Append(c.Id), "append c_id");
                    AppendDecimal(discount, ToDecimal(c.Discount));
                    ThrowArrow(credit.Append(c.Credit), "append c_credit");
                    ThrowArrow(last.Append(c.Last), "append c_last");
                    ThrowArrow(first.Append(c.First), "append c_first");
                    AppendDecimal(creditLim, ToDecimal(c.CreditLimit));
                    AppendDecimal(balance, ToDecimal(c.Balance));
                    AppendDecimal(ytdPayment, ToDecimal(c.YtdPayment));
                    ThrowArrow(paymentCnt.Append(c.PaymentCount), "append c_payment_cnt");
                    ThrowArrow(deliveryCnt.Append(c.DeliveryCount), "append c_delivery_cnt");
                    ThrowArrow(street1.Append(c.Street1), "append c_street_1");
                    ThrowArrow(street2.Append(c.Street2), "append c_street_2");
                    ThrowArrow(city.Append(c.City), "append c_city");
                    ThrowArrow(state.Append(c.State), "append c_state");
                    ThrowArrow(zip.Append(c.Zip), "append c_zip");
                    ThrowArrow(phone.Append(c.Phone), "append c_phone");
                    ThrowArrow(since.Append(ToTimestampMicros(c.SinceUnix)), "append c_since");
                    ThrowArrow(middle.Append(c.Middle), "append c_middle");
                    ThrowArrow(data.Append(c.Data), "append c_data");

                    auto h = NGenerator::GenerateHistory(seed, warehouseId, d, cid);
                    const int64_t nanoTs = h.DateUnix * 1000000000LL + cid;
                    ThrowArrow(hCWId.Append(h.CustomerWarehouseId), "append h_c_w_id");
                    ThrowArrow(hCDId.Append(h.CustomerDistrictId), "append h_c_d_id");
                    ThrowArrow(hCId.Append(h.CustomerId), "append h_c_id");
                    ThrowArrow(hNanoTs.Append(nanoTs), "append h_c_nano_ts");
                    ThrowArrow(hDId.Append(h.DistrictId), "append h_d_id");
                    ThrowArrow(hWId.Append(h.WarehouseId), "append h_w_id");
                    ThrowArrow(hDate.Append(ToTimestampMicros(h.DateUnix)), "append h_date");
                    AppendDecimal(hAmount, ToDecimal(h.Amount));
                    ThrowArrow(hData.Append(h.Data), "append h_data");
                }

                BulkUpsertArrow(connection, TABLE_CUSTOMER, MakeBatch(CustomerSchema(), {
                    Finish(cWId), Finish(cDId), Finish(cId), Finish(discount), Finish(credit),
                    Finish(last), Finish(first), Finish(creditLim), Finish(balance), Finish(ytdPayment),
                    Finish(paymentCnt), Finish(deliveryCnt), Finish(street1), Finish(street2),
                    Finish(city), Finish(state), Finish(zip), Finish(phone), Finish(since),
                    Finish(middle), Finish(data),
                }, rows));
                BulkUpsertArrow(connection, TABLE_HISTORY, MakeBatch(HistorySchema(), {
                    Finish(hCWId), Finish(hCDId), Finish(hCId), Finish(hNanoTs),
                    Finish(hDId), Finish(hWId), Finish(hDate), Finish(hAmount), Finish(hData),
                }, rows));
            }

            const auto customerIds = NGenerator::InitialOrderCustomerPermutation(seed, warehouseId, d);
            arrow::Int32Builder oWId, oDId, oId, oCId, oCarrierId, oOlCnt, oAllLocal;
            arrow::TimestampBuilder oEntry(TimestampType(), arrow::default_memory_pool());
            arrow::Int32Builder noWId, noDId, noOId;
            arrow::Int32Builder olWId, olDId, olOId, olNumber, olIId, olSupplyWId, olQuantity;
            arrow::TimestampBuilder olDelivery(TimestampType(), arrow::default_memory_pool());
            arrow::FixedSizeBinaryBuilder olAmount(DecimalBinaryType());
            arrow::StringBuilder olDistInfo;

            int64_t orderRows = 0;
            int64_t newOrderRows = 0;
            int64_t orderLineRows = 0;
            for (int oid = 1; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
                const int cid = customerIds[oid - 1];
                auto order = NGenerator::GenerateOrder(seed, warehouseId, d, oid, cid);
                ThrowArrow(oWId.Append(order.WarehouseId), "append o_w_id");
                ThrowArrow(oDId.Append(order.DistrictId), "append o_d_id");
                ThrowArrow(oId.Append(order.Id), "append o_id");
                ThrowArrow(oCId.Append(order.CustomerId), "append o_c_id");
                if (order.CarrierId) {
                    ThrowArrow(oCarrierId.Append(*order.CarrierId), "append o_carrier_id");
                } else {
                    ThrowArrow(oCarrierId.AppendNull(), "append null o_carrier_id");
                }
                ThrowArrow(oOlCnt.Append(order.OlCnt), "append o_ol_cnt");
                ThrowArrow(oAllLocal.Append(order.AllLocal), "append o_all_local");
                ThrowArrow(oEntry.Append(ToTimestampMicros(order.EntryUnix)), "append o_entry_d");
                ++orderRows;

                if (oid >= FIRST_UNPROCESSED_O_ID) {
                    auto no = NGenerator::GenerateNewOrder(warehouseId, d, oid);
                    ThrowArrow(noWId.Append(no.WarehouseId), "append no_w_id");
                    ThrowArrow(noDId.Append(no.DistrictId), "append no_d_id");
                    ThrowArrow(noOId.Append(no.OrderId), "append no_o_id");
                    ++newOrderRows;
                }

                const bool delivered = oid < FIRST_UNPROCESSED_O_ID;
                auto lines = NGenerator::GenerateOrderLines(
                    seed, warehouseId, d, oid, order.OlCnt,
                    delivered ? std::optional<int64_t>(order.EntryUnix) : std::nullopt);
                for (const auto& line : lines) {
                    ThrowArrow(olWId.Append(line.WarehouseId), "append ol_w_id");
                    ThrowArrow(olDId.Append(line.DistrictId), "append ol_d_id");
                    ThrowArrow(olOId.Append(line.OrderId), "append ol_o_id");
                    ThrowArrow(olNumber.Append(line.Number), "append ol_number");
                    ThrowArrow(olIId.Append(line.ItemId), "append ol_i_id");
                    if (line.DeliveryUnix) {
                        ThrowArrow(
                            olDelivery.Append(ToTimestampMicros(*line.DeliveryUnix)),
                            "append ol_delivery_d");
                    } else {
                        ThrowArrow(olDelivery.AppendNull(), "append null ol_delivery_d");
                    }
                    AppendDecimal(olAmount, ToDecimal(line.Amount));
                    ThrowArrow(olSupplyWId.Append(line.SupplyWarehouseId), "append ol_supply_w_id");
                    ThrowArrow(olQuantity.Append(line.Quantity), "append ol_quantity");
                    ThrowArrow(olDistInfo.Append(line.DistInfo), "append ol_dist_info");
                    ++orderLineRows;
                }
            }

            BulkUpsertArrow(connection, TABLE_OORDER, MakeBatch(OorderSchema(), {
                Finish(oWId), Finish(oDId), Finish(oId), Finish(oCId), Finish(oCarrierId),
                Finish(oOlCnt), Finish(oAllLocal), Finish(oEntry),
            }, orderRows));
            BulkUpsertArrow(connection, TABLE_NEW_ORDER, MakeBatch(NewOrderSchema(), {
                Finish(noWId), Finish(noDId), Finish(noOId),
            }, newOrderRows));
            BulkUpsertArrow(connection, TABLE_ORDER_LINE, MakeBatch(OrderLineSchema(), {
                Finish(olWId), Finish(olDId), Finish(olOId), Finish(olNumber), Finish(olIId),
                Finish(olDelivery), Finish(olAmount), Finish(olSupplyWId), Finish(olQuantity),
                Finish(olDistInfo),
            }, orderLineRows));
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

#include "ydb_session.h"

#include <log.h>
#include <money.h>

#include <fmt/format.h>
#include <util/datetime/base.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/params/params.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace NTpcc {

namespace {

using NYdb::TDecimalValue;
using NYdb::TParams;
using NYdb::TParamsBuilder;
using NYdb::NQuery::TExecuteQueryResult;
using NYdb::NQuery::TSession;
using NYdb::NQuery::TTxControl;
using NYdb::NQuery::TTxSettings;

constexpr uint8_t MONEY_PRECISION = 22;
constexpr uint8_t MONEY_SCALE = 9;

TOperationResult FailOp(EErrorClass cls, std::string message, std::string code = {}) {
    TOperationResult r;
    r.Ok = false;
    r.ErrorClass = cls;
    r.Message = std::move(message);
    r.NativeCode = std::move(code);
    return r;
}

TOperationResult OkOp(size_t expected, size_t actual, TOperationPayload payload = {}) {
    TOperationResult r;
    r.Ok = true;
    r.ExpectedRows = expected;
    r.ActualRows = actual;
    r.ErrorClass = EErrorClass::Permanent;
    r.Payload = std::move(payload);
    return r;
}

TFuture<TOperationResult> ReadyOp(TOperationResult result) {
    TPromise<TOperationResult> p;
    auto f = p.GetFuture();
    p.SetValue(std::move(result));
    return f;
}

TFuture<TCommitResult> ReadyCommit(TCommitResult result) {
    TPromise<TCommitResult> p;
    auto f = p.GetFuture();
    p.SetValue(std::move(result));
    return f;
}

TFuture<TBatchResult> ReadyBatch(TBatchResult result) {
    TPromise<TBatchResult> p;
    auto f = p.GetFuture();
    p.SetValue(std::move(result));
    return f;
}

TFuture<TFinalCommitResult> ReadyFinal(TFinalCommitResult result) {
    TPromise<TFinalCommitResult> p;
    auto f = p.GetFuture();
    p.SetValue(std::move(result));
    return f;
}

std::string TimestampToString(const TInstant& ts) {
    return std::string(ts.ToString());
}

std::string Prefix(const std::string& path) {
    return fmt::format("PRAGMA TablePathPrefix(\"{}\");\n", path);
}

TDecimalValue Decimal(TMoney value) {
    return TDecimalValue(value.ToString(), MONEY_PRECISION, MONEY_SCALE);
}

TMoney ParseMoney(NYdb::TResultSetParser& parser, const char* column) {
    return TMoney::Parse(parser.ColumnParser(column).GetDecimal().ToString());
}

TRate ParseRate(NYdb::TResultSetParser& parser, const char* column) {
    return TRate::Parse(parser.ColumnParser(column).GetDecimal().ToString());
}

std::string ParseUtf8(NYdb::TResultSetParser& parser, const char* column) {
    return std::string(*parser.ColumnParser(column).GetOptionalUtf8());
}

TCustomerRow ParseCustomer(NYdb::TResultSetParser& parser) {
    TCustomerRow cust;
    cust.CustomerID = parser.ColumnParser("c_id").GetInt32();
    cust.First = ParseUtf8(parser, "c_first");
    cust.Middle = ParseUtf8(parser, "c_middle");
    cust.Last = ParseUtf8(parser, "c_last");
    cust.Street1 = ParseUtf8(parser, "c_street_1");
    cust.Street2 = ParseUtf8(parser, "c_street_2");
    cust.City = ParseUtf8(parser, "c_city");
    cust.State = ParseUtf8(parser, "c_state");
    cust.Zip = ParseUtf8(parser, "c_zip");
    cust.Phone = ParseUtf8(parser, "c_phone");
    cust.Credit = ParseUtf8(parser, "c_credit");
    cust.CreditLimit = ParseMoney(parser, "c_credit_lim");
    cust.Discount = ParseRate(parser, "c_discount");
    cust.Balance = ParseMoney(parser, "c_balance");
    cust.YtdPayment = ParseMoney(parser, "c_ytd_payment");
    cust.PaymentCount = parser.ColumnParser("c_payment_cnt").GetInt32();
    cust.DeliveryCount = parser.ColumnParser("c_delivery_cnt").GetInt32();
    cust.Data = ParseUtf8(parser, "c_data");
    if (auto ts = parser.ColumnParser("c_since").GetOptionalTimestamp()) {
        cust.Since = TimestampToString(*ts);
    }
    return cust;
}

int64_t HistoryNanoTs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // anonymous

TYdbTpccTransaction::TYdbTpccTransaction(TSession session, std::string path)
    : Session_(std::move(session))
    , Path_(std::move(path))
{}

TFuture<TCommitResult> TYdbTpccTransaction::Commit() {
    if (Terminal_) {
        return ReadyCommit({ECommitOutcome::OutcomeUnknown, EErrorClass::Permanent, {}, "Commit called in terminal state"});
    }
    if (!Tx_) {
        Terminal_ = true;
        return ReadyCommit({ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}});
    }
    try {
        auto status = Tx_->Commit().GetValueSync();
        Terminal_ = true;
        if (status.IsSuccess()) {
            return ReadyCommit({ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}});
        }
        const auto cls = Classifier_.ClassifyStatus(status, true);
        return ReadyCommit({
            cls == EErrorClass::AmbiguousCommit ? ECommitOutcome::OutcomeUnknown : ECommitOutcome::RolledBack,
            cls,
            YdbStatusCodeOf(status.GetStatus()),
            YdbIssuesToString(status)});
    } catch (const std::exception& ex) {
        Terminal_ = true;
        const auto cls = Classifier_.ClassifyException(ex, true);
        return ReadyCommit({
            cls == EErrorClass::AmbiguousCommit ? ECommitOutcome::OutcomeUnknown : ECommitOutcome::RolledBack,
            cls,
            {},
            ex.what()});
    }
}

TFuture<TCommitResult> TYdbTpccTransaction::Rollback() {
    if (Terminal_) {
        return ReadyCommit({ECommitOutcome::OutcomeUnknown, EErrorClass::Permanent, {}, "Rollback called in terminal state"});
    }
    if (!Tx_) {
        Terminal_ = true;
        return ReadyCommit({ECommitOutcome::RolledBack, EErrorClass::Permanent, {}, {}});
    }
    try {
        auto status = Tx_->Rollback().GetValueSync();
        Terminal_ = true;
        if (status.IsSuccess()) {
            return ReadyCommit({ECommitOutcome::RolledBack, EErrorClass::Permanent, {}, {}});
        }
        return ReadyCommit({
            ECommitOutcome::OutcomeUnknown,
            Classifier_.ClassifyStatus(status),
            YdbStatusCodeOf(status.GetStatus()),
            YdbIssuesToString(status)});
    } catch (const std::exception& ex) {
        Terminal_ = true;
        return ReadyCommit({ECommitOutcome::OutcomeUnknown, Classifier_.ClassifyException(ex), {}, ex.what()});
    }
}

TFuture<TCommitResult> TYdbTpccTransaction::Cancel() {
    auto result = Rollback().Get();
    result.ErrorClass = EErrorClass::Cancelled;
    return ReadyCommit(std::move(result));
}

TFuture<TBatchResult> TYdbTpccTransaction::ExecuteBatch(const std::vector<TSemanticOp>& ops) {
    TBatchResult batch;
    batch.Ok = true;
    for (const auto& op : ops) {
        auto one = Execute(op).Get();
        batch.Results.push_back(one);
        if (!one.Ok) {
            batch.Ok = false;
            batch.ErrorClass = one.ErrorClass;
            batch.NativeCode = one.NativeCode;
            batch.Message = one.Message;
            break;
        }
    }
    return ReadyBatch(std::move(batch));
}

TFuture<TFinalCommitResult> TYdbTpccTransaction::ExecuteFinalAndCommit(const TSemanticOp& op) {
    TFinalCommitResult out;
    FinalCommitMode_ = true;
    out.Operation = Execute(op).Get();
    FinalCommitMode_ = false;
    if (!out.Operation.Ok) {
        out.Commit = Rollback().Get();
        return ReadyFinal(std::move(out));
    }
    if (Terminal_) {
        out.Commit = {ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}};
        return ReadyFinal(std::move(out));
    }
    out.Commit = Commit().Get();
    return ReadyFinal(std::move(out));
}

TFuture<TOperationResult> TYdbTpccTransaction::ExecuteSelect1() {
    if (Terminal_) {
        return ReadyOp(FailOp(EErrorClass::Permanent, "ExecuteSelect1 called in terminal state"));
    }
    try {
        TTxControl txControl = Tx_
            ? TTxControl::Tx(*Tx_)
            : TTxControl::BeginTx(TTxSettings::SerializableRW());
        auto result = Session_.ExecuteQuery("SELECT 1 AS one;", txControl).GetValueSync();
        if (!result.IsSuccess()) {
            return ReadyOp(FailOp(
                Classifier_.ClassifyStatus(result),
                YdbIssuesToString(result),
                YdbStatusCodeOf(result.GetStatus())));
        }
        if (auto tx = result.GetTransaction()) {
            Tx_ = std::move(*tx);
        }
        return ReadyOp(OkOp(1, 1));
    } catch (const std::exception& ex) {
        return ReadyOp(FailOp(Classifier_.ClassifyException(ex), ex.what()));
    }
}

TFuture<TOperationResult> TYdbTpccTransaction::Execute(const TSemanticOp& op) {
    if (Terminal_) {
        return ReadyOp(FailOp(EErrorClass::Permanent, "Execute called in terminal state"));
    }

    auto exec = [&](const std::string& query, std::optional<TParams> params = std::nullopt, bool commit = false) {
        TTxControl txControl = Tx_
            ? TTxControl::Tx(*Tx_)
            : TTxControl::BeginTx(TTxSettings::SerializableRW());
        if (commit) {
            txControl.CommitTx(true);
        }

        auto result = !params
            ? Session_.ExecuteQuery(query, txControl).GetValueSync()
            : Session_.ExecuteQuery(query, txControl, *params).GetValueSync();
        if (!result.IsSuccess()) {
            throw NYdb::NStatusHelpers::TYdbErrorException(std::move(result));
        }
        if (commit) {
            Terminal_ = true;
            Tx_.reset();
        } else if (auto tx = result.GetTransaction()) {
            Tx_ = std::move(*tx);
        }
        return result;
    };

    try {
        if (const auto* p = std::get_if<TGetWarehouseTax>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                SELECT w_tax FROM `warehouse` WHERE w_id = $w_id;
            )", std::move(params));
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            if (!parser.TryNextRow()) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "warehouse not found"));
            }
            return ReadyOp(OkOp(1, 1, ParseRate(parser, "w_tax")));
        }

        if (const auto* p = std::get_if<TReserveDistrictOrderId>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                SELECT d_next_o_id, d_tax
                  FROM `district`
                 WHERE d_w_id = $w_id AND d_id = $d_id;
            )", std::move(params));
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            if (!parser.TryNextRow()) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "district not found"));
            }
            const int nextId = parser.ColumnParser("d_next_o_id").GetInt32();
            const TRate tax = ParseRate(parser, "d_tax");
            params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$next_o_id").Int32(nextId + 1).Build()
                .Build();
            exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $next_o_id AS Int32;
                UPDATE `district`
                   SET d_next_o_id = $next_o_id
                 WHERE d_w_id = $w_id AND d_id = $d_id;
            )", std::move(params));
            TDistrictOrderReservation res;
            res.NextOrderID = nextId;
            res.DistrictTax = tax;
            return ReadyOp(OkOp(1, 1, res));
        }

        if (const auto* p = std::get_if<TGetCustomerById>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                SELECT c_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state,
                       c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment,
                       c_payment_cnt, c_delivery_cnt, c_data, c_since
                  FROM `customer`
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
            )", std::move(params));
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            if (!parser.TryNextRow()) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "customer not found"));
            }
            return ReadyOp(OkOp(1, 1, ParseCustomer(parser)));
        }

        if (const auto* p = std::get_if<TGetCustomersByLastName>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_last").Utf8(p->LastName).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_last AS Utf8;
                SELECT c_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state,
                       c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment,
                       c_payment_cnt, c_delivery_cnt, c_data, c_since
                  FROM `customer` VIEW `idx_customer_name` AS idx
                 WHERE idx.c_w_id = $w_id AND idx.c_d_id = $d_id AND idx.c_last = $c_last
                 ORDER BY idx.c_first;
            )", std::move(params));
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            std::vector<TCustomerRow> customers;
            while (parser.TryNextRow()) {
                customers.push_back(ParseCustomer(parser));
            }
            return ReadyOp(OkOp(customers.size(), customers.size(), std::move(customers)));
        }

        if (const auto* p = std::get_if<TGetItems>(&op)) {
            TParamsBuilder builder;
            auto& params = builder.AddParam("$item_ids").BeginList();
            for (int id : p->ItemIDs) {
                params.AddListItem().Int32(id);
            }
            auto builtParams = params.EndList().Build().Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $item_ids AS List<Int32>;
                SELECT i_id, i_price, i_name, i_data
                  FROM `item`
                 WHERE i_id IN $item_ids
                 ORDER BY i_id;
            )", std::move(builtParams));
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            std::vector<TItemRow> items;
            while (parser.TryNextRow()) {
                TItemRow item;
                item.ItemID = parser.ColumnParser("i_id").GetInt32();
                item.Price = ParseMoney(parser, "i_price");
                item.Name = ParseUtf8(parser, "i_name");
                item.Data = ParseUtf8(parser, "i_data");
                items.push_back(std::move(item));
            }
            if (items.size() != p->ItemIDs.size()) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "item not found"));
            }
            return ReadyOp(OkOp(p->ItemIDs.size(), items.size(), std::move(items)));
        }

        if (const auto* p = std::get_if<TCreateOrder>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$o_id").Int32(p->OrderID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .AddParam("$ol_cnt").Int32(p->LineCount).Build()
                .AddParam("$all_local").Int32(p->AllLocal).Build()
                .Build();
            exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                DECLARE $c_id AS Int32;
                DECLARE $ol_cnt AS Int32;
                DECLARE $all_local AS Int32;
                UPSERT INTO `oorder` (o_w_id, o_d_id, o_id, o_c_id, o_carrier_id, o_ol_cnt, o_all_local, o_entry_d)
                VALUES ($w_id, $d_id, $o_id, $c_id, NULL, $ol_cnt, $all_local, CurrentUtcTimestamp());
                UPSERT INTO `new_order` (no_w_id, no_d_id, no_o_id)
                VALUES ($w_id, $d_id, $o_id);
            )", std::move(params));
            return ReadyOp(OkOp(2, 2));
        }

        if (const auto* p = std::get_if<TGetStocksForUpdate>(&op)) {
            TParamsBuilder builder;
            auto& params = builder
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$stocks").BeginList();
            for (const auto& key : p->Stocks) {
                params.AddListItem()
                    .BeginStruct()
                    .AddMember("s_w_id").Int32(key.WarehouseID)
                    .AddMember("s_i_id").Int32(key.ItemID)
                    .EndStruct();
            }
            auto builtParams = params.EndList().Build().Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $d_id AS Int32;
                DECLARE $stocks AS List<Struct<s_w_id: Int32, s_i_id: Int32>>;
                SELECT s.s_w_id, s.s_i_id, s.s_quantity, s.s_ytd, s.s_order_cnt, s.s_remote_cnt, s.s_data,
                       CASE $d_id
                         WHEN 1 THEN s.s_dist_01 WHEN 2 THEN s.s_dist_02 WHEN 3 THEN s.s_dist_03
                         WHEN 4 THEN s.s_dist_04 WHEN 5 THEN s.s_dist_05 WHEN 6 THEN s.s_dist_06
                         WHEN 7 THEN s.s_dist_07 WHEN 8 THEN s.s_dist_08 WHEN 9 THEN s.s_dist_09
                         ELSE s.s_dist_10
                       END AS s_dist_info
                  FROM AS_TABLE($stocks) AS k
                  INNER JOIN `stock` AS s
                     ON s.s_w_id = k.s_w_id AND s.s_i_id = k.s_i_id;
            )", std::move(builtParams));
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            std::vector<TStockRow> stocks;
            while (parser.TryNextRow()) {
                TStockRow row;
                row.WarehouseID = parser.ColumnParser("s_w_id").GetInt32();
                row.ItemID = parser.ColumnParser("s_i_id").GetInt32();
                row.Quantity = parser.ColumnParser("s_quantity").GetInt32();
                row.Ytd = ParseMoney(parser, "s_ytd");
                row.OrderCount = parser.ColumnParser("s_order_cnt").GetInt32();
                row.RemoteCount = parser.ColumnParser("s_remote_cnt").GetInt32();
                row.Data = ParseUtf8(parser, "s_data");
                row.DistInfo = ParseUtf8(parser, "s_dist_info");
                stocks.push_back(std::move(row));
            }
            if (stocks.size() != p->Stocks.size()) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "stock not found"));
            }
            return ReadyOp(OkOp(p->Stocks.size(), stocks.size(), std::move(stocks)));
        }

        if (const auto* p = std::get_if<TUpdateStock>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$i_id").Int32(p->ItemID).Build()
                .AddParam("$quantity").Int32(p->NewQuantity).Build()
                .AddParam("$ordered").Decimal(TDecimalValue(std::to_string(p->OrderedQuantity), MONEY_PRECISION, MONEY_SCALE)).Build()
                .AddParam("$remote").Int32(p->RemoteIncrement).Build()
                .Build();
            exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $i_id AS Int32;
                DECLARE $quantity AS Int32;
                DECLARE $ordered AS Decimal(22,9);
                DECLARE $remote AS Int32;
                UPDATE `stock`
                   SET s_quantity = $quantity,
                       s_ytd = s_ytd + $ordered,
                       s_order_cnt = s_order_cnt + 1,
                       s_remote_cnt = s_remote_cnt + $remote
                 WHERE s_w_id = $w_id AND s_i_id = $i_id;
            )", std::move(params));
            return ReadyOp(OkOp(1, 1));
        }

        if (const auto* p = std::get_if<TInsertOrderLine>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$o_id").Int32(p->OrderID).Build()
                .AddParam("$number").Int32(p->LineNumber).Build()
                .AddParam("$i_id").Int32(p->ItemID).Build()
                .AddParam("$supply_w_id").Int32(p->SupplyWarehouseID).Build()
                .AddParam("$quantity").Int32(p->Quantity).Build()
                .AddParam("$amount").Decimal(Decimal(p->Amount)).Build()
                .AddParam("$dist_info").Utf8(p->DistInfo).Build()
                .Build();
            exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                DECLARE $number AS Int32;
                DECLARE $i_id AS Int32;
                DECLARE $supply_w_id AS Int32;
                DECLARE $quantity AS Int32;
                DECLARE $amount AS Decimal(22,9);
                DECLARE $dist_info AS Utf8;
                UPSERT INTO `order_line` (ol_w_id, ol_d_id, ol_o_id, ol_number, ol_i_id, ol_delivery_d,
                                          ol_amount, ol_supply_w_id, ol_quantity, ol_dist_info)
                VALUES ($w_id, $d_id, $o_id, $number, $i_id, NULL, $amount, $supply_w_id, $quantity, $dist_info);
            )", std::move(params), FinalCommitMode_);
            return ReadyOp(OkOp(1, 1));
        }

        if (const auto* p = std::get_if<TApplyPaymentToLocation>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$amount").Decimal(Decimal(p->Amount)).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $amount AS Decimal(22,9);
                UPDATE `warehouse` SET w_ytd = w_ytd + $amount WHERE w_id = $w_id;
                UPDATE `district` SET d_ytd = d_ytd + $amount WHERE d_w_id = $w_id AND d_id = $d_id;
                SELECT w_name, w_street_1, w_street_2, w_city, w_state, w_zip
                  FROM `warehouse` WHERE w_id = $w_id;
                SELECT d_name, d_street_1, d_street_2, d_city, d_state, d_zip
                  FROM `district` WHERE d_w_id = $w_id AND d_id = $d_id;
            )", std::move(params));
            TWarehouseDistrictInfo info;
            NYdb::TResultSetParser wh(result.GetResultSet(0));
            NYdb::TResultSetParser dist(result.GetResultSet(1));
            if (!wh.TryNextRow() || !dist.TryNextRow()) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "warehouse or district not found"));
            }
            info.WarehouseName = ParseUtf8(wh, "w_name");
            info.WarehouseStreet1 = ParseUtf8(wh, "w_street_1");
            info.WarehouseStreet2 = ParseUtf8(wh, "w_street_2");
            info.WarehouseCity = ParseUtf8(wh, "w_city");
            info.WarehouseState = ParseUtf8(wh, "w_state");
            info.WarehouseZip = ParseUtf8(wh, "w_zip");
            info.DistrictName = ParseUtf8(dist, "d_name");
            info.DistrictStreet1 = ParseUtf8(dist, "d_street_1");
            info.DistrictStreet2 = ParseUtf8(dist, "d_street_2");
            info.DistrictCity = ParseUtf8(dist, "d_city");
            info.DistrictState = ParseUtf8(dist, "d_state");
            info.DistrictZip = ParseUtf8(dist, "d_zip");
            return ReadyOp(OkOp(2, 2, std::move(info)));
        }

        if (const auto* p = std::get_if<TGetCustomerData>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                SELECT c_data FROM `customer`
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
            )", std::move(params));
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            if (!parser.TryNextRow()) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "customer not found"));
            }
            return ReadyOp(OkOp(1, 1, ParseUtf8(parser, "c_data")));
        }

        if (const auto* p = std::get_if<TUpdateCustomerPayment>(&op)) {
            TParamsBuilder builder;
            builder
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .AddParam("$balance").Decimal(Decimal(p->NewBalance)).Build()
                .AddParam("$ytd_payment").Decimal(Decimal(p->NewYtdPayment)).Build()
                .AddParam("$payment_cnt").Int32(p->NewPaymentCount).Build();
            std::string query = Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                DECLARE $balance AS Decimal(22,9);
                DECLARE $ytd_payment AS Decimal(22,9);
                DECLARE $payment_cnt AS Int32;
            )";
            if (p->UpdateData) {
                auto params = builder.AddParam("$data").Utf8(p->NewData).Build().Build();
                exec(query + R"(
                    DECLARE $data AS Utf8;
                    UPDATE `customer`
                       SET c_balance = $balance, c_ytd_payment = $ytd_payment,
                           c_payment_cnt = $payment_cnt, c_data = $data
                     WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
                )", std::move(params));
            } else {
                auto params = builder.Build();
                exec(query + R"(
                    UPDATE `customer`
                       SET c_balance = $balance, c_ytd_payment = $ytd_payment,
                           c_payment_cnt = $payment_cnt
                     WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
                )", std::move(params));
            }
            return ReadyOp(OkOp(1, 1));
        }

        if (const auto* p = std::get_if<TInsertPaymentHistory>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$h_c_w_id").Int32(p->CustomerWarehouseID).Build()
                .AddParam("$h_c_d_id").Int32(p->CustomerDistrictID).Build()
                .AddParam("$h_c_id").Int32(p->CustomerID).Build()
                .AddParam("$h_c_nano_ts").Int64(HistoryNanoTs()).Build()
                .AddParam("$h_w_id").Int32(p->PaymentWarehouseID).Build()
                .AddParam("$h_d_id").Int32(p->PaymentDistrictID).Build()
                .AddParam("$h_amount").Decimal(Decimal(p->Amount)).Build()
                .AddParam("$h_data").Utf8(p->Data).Build()
                .Build();
            exec(Prefix(Path_) + R"(
                DECLARE $h_c_w_id AS Int32;
                DECLARE $h_c_d_id AS Int32;
                DECLARE $h_c_id AS Int32;
                DECLARE $h_c_nano_ts AS Int64;
                DECLARE $h_w_id AS Int32;
                DECLARE $h_d_id AS Int32;
                DECLARE $h_amount AS Decimal(22,9);
                DECLARE $h_data AS Utf8;
                UPSERT INTO `history` (h_c_w_id, h_c_d_id, h_c_id, h_c_nano_ts, h_d_id, h_w_id, h_date, h_amount, h_data)
                VALUES ($h_c_w_id, $h_c_d_id, $h_c_id, $h_c_nano_ts, $h_d_id, $h_w_id,
                        CurrentUtcTimestamp(), $h_amount, $h_data);
            )", std::move(params), FinalCommitMode_);
            return ReadyOp(OkOp(1, 1));
        }

        if (const auto* p = std::get_if<TGetLatestCustomerOrder>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                SELECT o_id, o_c_id, o_carrier_id, o_entry_d
                  FROM `oorder` VIEW `idx_order` AS idx
                 WHERE idx.o_w_id = $w_id AND idx.o_d_id = $d_id AND idx.o_c_id = $c_id
                 ORDER BY idx.o_id DESC LIMIT 1;
            )", std::move(params));
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            if (!parser.TryNextRow()) {
                return ReadyOp(OkOp(0, 0));
            }
            TOrderHeader header;
            header.OrderID = parser.ColumnParser("o_id").GetInt32();
            header.CustomerID = parser.ColumnParser("o_c_id").GetInt32();
            if (auto carrier = parser.ColumnParser("o_carrier_id").GetOptionalInt32()) {
                header.CarrierID = *carrier;
            }
            if (auto ts = parser.ColumnParser("o_entry_d").GetOptionalTimestamp()) {
                header.EntryDate = TimestampToString(*ts);
            }
            return ReadyOp(OkOp(1, 1, std::move(header)));
        }

        if (const auto* p = std::get_if<TGetOrderStatusLines>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$o_id").Int32(p->OrderID).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d
                  FROM `order_line`
                 WHERE ol_w_id = $w_id AND ol_d_id = $d_id AND ol_o_id = $o_id
                 ORDER BY ol_number;
            )", std::move(params), FinalCommitMode_);
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            std::vector<TOrderStatusLine> lines;
            while (parser.TryNextRow()) {
                TOrderStatusLine line;
                line.ItemID = parser.ColumnParser("ol_i_id").GetInt32();
                line.SupplyWarehouseID = parser.ColumnParser("ol_supply_w_id").GetInt32();
                line.Quantity = parser.ColumnParser("ol_quantity").GetInt32();
                line.Amount = ParseMoney(parser, "ol_amount");
                if (auto ts = parser.ColumnParser("ol_delivery_d").GetOptionalTimestamp()) {
                    line.DeliveryDate = TimestampToString(*ts);
                }
                lines.push_back(std::move(line));
            }
            return ReadyOp(OkOp(lines.size(), lines.size(), std::move(lines)));
        }

        if (const auto* p = std::get_if<TGetOldestNewOrder>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                SELECT no_o_id
                  FROM `new_order`
                 WHERE no_w_id = $w_id AND no_d_id = $d_id
                 ORDER BY no_o_id ASC LIMIT 1;
            )", std::move(params));
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            if (!parser.TryNextRow()) {
                return ReadyOp(OkOp(0, 0, 0));
            }
            return ReadyOp(OkOp(1, 1, parser.ColumnParser("no_o_id").GetInt32()));
        }

        if (const auto* p = std::get_if<TGetDeliveryOrderInfo>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$o_id").Int32(p->OrderID).Build()
                .Build();
            auto result = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                SELECT o_c_id FROM `oorder`
                 WHERE o_w_id = $w_id AND o_d_id = $d_id AND o_id = $o_id;
                SELECT ol_amount FROM `order_line`
                 WHERE ol_w_id = $w_id AND ol_d_id = $d_id AND ol_o_id = $o_id;
            )", std::move(params));
            NYdb::TResultSetParser order(result.GetResultSet(0));
            if (!order.TryNextRow()) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "order not found"));
            }
            TDeliveryOrderInfo info;
            info.CustomerID = order.ColumnParser("o_c_id").GetInt32();
            NYdb::TResultSetParser lines(result.GetResultSet(1));
            int64_t totalCents = 0;
            while (lines.TryNextRow()) {
                totalCents += ParseMoney(lines, "ol_amount").Cents();
                ++info.LineCount;
            }
            info.TotalAmount = TMoney::FromCents(totalCents);
            return ReadyOp(OkOp(1, 1, std::move(info)));
        }

        if (const auto* p = std::get_if<TCompleteOrderDelivery>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$o_id").Int32(p->OrderID).Build()
                .AddParam("$carrier_id").Int32(p->CarrierID).Build()
                .Build();
            exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                DECLARE $carrier_id AS Int32;
                DELETE FROM `new_order`
                 WHERE no_w_id = $w_id AND no_d_id = $d_id AND no_o_id = $o_id;
                UPDATE `oorder`
                   SET o_carrier_id = $carrier_id
                 WHERE o_w_id = $w_id AND o_d_id = $d_id AND o_id = $o_id;
                UPDATE `order_line`
                   SET ol_delivery_d = CurrentUtcTimestamp()
                 WHERE ol_w_id = $w_id AND ol_d_id = $d_id AND ol_o_id = $o_id;
            )", std::move(params));
            return ReadyOp(OkOp(3, 3));
        }

        if (const auto* p = std::get_if<TApplyDeliveryToCustomer>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .AddParam("$amount").Decimal(Decimal(p->Amount)).Build()
                .Build();
            exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                DECLARE $amount AS Decimal(22,9);
                UPDATE `customer`
                   SET c_balance = c_balance + $amount,
                       c_delivery_cnt = c_delivery_cnt + 1
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
            )", std::move(params), FinalCommitMode_);
            return ReadyOp(OkOp(1, 1));
        }

        if (const auto* p = std::get_if<TCountRecentLowStock>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .Build();
            auto dist = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                SELECT d_next_o_id FROM `district`
                 WHERE d_w_id = $w_id AND d_id = $d_id;
            )", std::move(params));
            NYdb::TResultSetParser distParser(dist.GetResultSet(0));
            if (!distParser.TryNextRow()) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "district not found"));
            }
            const int nextOid = distParser.ColumnParser("d_next_o_id").GetInt32();
            params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$max_o_id").Int32(nextOid).Build()
                .AddParam("$min_o_id").Int32(nextOid - p->RecentOrderCount).Build()
                .AddParam("$threshold").Int32(p->Threshold).Build()
                .Build();
            auto stock = exec(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $max_o_id AS Int32;
                DECLARE $min_o_id AS Int32;
                DECLARE $threshold AS Int32;
                SELECT COUNT(DISTINCT s.s_i_id) AS low_stock
                  FROM `order_line` AS ol
                  INNER JOIN `stock` AS s
                     ON s.s_w_id = $w_id AND s.s_i_id = ol.ol_i_id
                 WHERE ol.ol_w_id = $w_id AND ol.ol_d_id = $d_id
                   AND ol.ol_o_id < $max_o_id AND ol.ol_o_id >= $min_o_id
                   AND s.s_quantity < $threshold;
            )", std::move(params), FinalCommitMode_);
            NYdb::TResultSetParser stockParser(stock.GetResultSet(0));
            int count = 0;
            if (stockParser.TryNextRow()) {
                count = static_cast<int>(stockParser.ColumnParser("low_stock").GetUint64());
            }
            return ReadyOp(OkOp(1, 1, count));
        }

        return ReadyOp(FailOp(EErrorClass::Permanent, "semantic op not yet bound in TYdbTpccTransaction"));
    } catch (const NYdb::NStatusHelpers::TYdbErrorException& ex) {
        const auto& status = ex.GetStatus();
        return ReadyOp(FailOp(
            Classifier_.ClassifyStatus(status),
            YdbIssuesToString(status),
            YdbStatusCodeOf(status.GetStatus())));
    } catch (const std::exception& ex) {
        return ReadyOp(FailOp(Classifier_.ClassifyException(ex), ex.what()));
    }
}

TYdbTpccSession::TYdbTpccSession(TSession session, std::string path)
    : Session_(std::move(session))
    , Path_(std::move(path))
{}

TFuture<std::unique_ptr<ITpccTransaction>> TYdbTpccSession::Begin(EIsolationLevel /*isolation*/) {
    TPromise<std::unique_ptr<ITpccTransaction>> promise;
    auto future = promise.GetFuture();
    promise.SetValue(std::make_unique<TYdbTpccTransaction>(Session_, Path_));
    return future;
}

TYdbSessionFactory::TYdbSessionFactory(TYdbConnection& connection)
    : Connection_(connection)
{}

std::unique_ptr<ITpccSession> TYdbSessionFactory::CreateSession() {
    auto result = Connection_.QueryClient().GetSession().GetValueSync();
    if (!result.IsSuccess()) {
        throw NYdb::NStatusHelpers::TYdbErrorException(std::move(result));
    }
    return std::make_unique<TYdbTpccSession>(
        result.GetSession(), Connection_.AbsolutePathPrefix());
}

} // namespace NTpcc

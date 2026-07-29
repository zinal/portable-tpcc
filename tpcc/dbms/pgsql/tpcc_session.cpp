#include "tpcc_session.h"

#include "query_result.h"

#include <log.h>
#include <money.h>

#include <stdexcept>
#include <utility>
#include <variant>

namespace NTpcc {

namespace {

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

TMoney MoneyFromDouble(double v) {
    // Transitional: PG query path still surfaces doubles; convert to cents.
    auto cents = static_cast<int64_t>(v * 100.0 + (v >= 0 ? 0.5 : -0.5));
    return TMoney::FromCents(cents);
}

TRate RateFromDouble(double v) {
    auto units = static_cast<int64_t>(v * 10000.0 + (v >= 0 ? 0.5 : -0.5));
    return TRate::FromUnits(units);
}

bool NextRow(QueryResult& result) {
    return result.TryNextRow();
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

// PgSession futures complete on the IO executor. Callers of ITpccTransaction
// must use TSuspendWithFuture when awaiting the outer TFuture; inside the
// adapter we block with Get() so we never resume a task-queue coroutine on
// an IO thread (see coro_traits.h).

} // namespace

TPgTpccTransaction::TPgTpccTransaction(PgSession& session)
    : Session_(session)
{}

bool TPgTpccTransaction::TerminalState() const {
    return Terminal_;
}

TFuture<TCommitResult> TPgTpccTransaction::Commit() {
    if (Terminal_) {
        return ReadyCommit({
            ECommitOutcome::OutcomeUnknown,
            EErrorClass::Permanent,
            {},
            "Commit called in terminal state"});
    }
    try {
        Session_.Commit().Get();
        Terminal_ = true;
        return ReadyCommit({ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}});
    } catch (const std::exception& ex) {
        Terminal_ = true;
        const auto cls = Classifier_.ClassifyCommitException(ex);
        return ReadyCommit({
            cls == EErrorClass::AmbiguousCommit ? ECommitOutcome::OutcomeUnknown
                                                : ECommitOutcome::RolledBack,
            cls,
            PgSqlStateOf(ex),
            ex.what()});
    }
}

TFuture<TCommitResult> TPgTpccTransaction::Rollback() {
    if (Terminal_) {
        return ReadyCommit({
            ECommitOutcome::RolledBack,
            EErrorClass::Permanent,
            {},
            "Rollback called in terminal state"});
    }
    try {
        Session_.Rollback().Get();
        Terminal_ = true;
        return ReadyCommit({ECommitOutcome::RolledBack, EErrorClass::Permanent, {}, {}});
    } catch (const std::exception& ex) {
        Terminal_ = true;
        return ReadyCommit({
            ECommitOutcome::RolledBack,
            Classifier_.ClassifyException(ex),
            PgSqlStateOf(ex),
            ex.what()});
    }
}

TFuture<TCommitResult> TPgTpccTransaction::Cancel() {
    auto result = Rollback().Get();
    result.ErrorClass = EErrorClass::Cancelled;
    return ReadyCommit(std::move(result));
}

TFuture<TBatchResult> TPgTpccTransaction::ExecuteBatch(const std::vector<TSemanticOp>& ops) {
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

TFuture<TFinalCommitResult> TPgTpccTransaction::ExecuteFinalAndCommit(const TSemanticOp& op) {
    TFinalCommitResult out;
    out.Operation = Execute(op).Get();
    if (!out.Operation.Ok) {
        out.Commit = Rollback().Get();
        return ReadyFinal(std::move(out));
    }
    out.Commit = Commit().Get();
    return ReadyFinal(std::move(out));
}

TFuture<TOperationResult> TPgTpccTransaction::Execute(const TSemanticOp& op) {
    if (Terminal_) {
        return ReadyOp(FailOp(EErrorClass::Permanent, "Execute called in terminal state"));
    }
    try {
        if (const auto* p = std::get_if<TGetWarehouseTax>(&op)) {
            auto result = Session_.ExecuteQuery(
                "SELECT w_tax FROM warehouse WHERE w_id = $1", p->WarehouseID).Get();
            if (!NextRow(result)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "warehouse not found", {}));
            }
            return ReadyOp(OkOp(1, 1, RateFromDouble(result.GetDouble(0))));
        }
        if (const auto* p = std::get_if<TReserveDistrictOrderId>(&op)) {
            auto result = Session_.ExecuteQuery(
                "SELECT d_next_o_id, d_tax FROM district "
                "WHERE d_w_id = $1 AND d_id = $2 FOR UPDATE",
                p->WarehouseID, p->DistrictID).Get();
            if (!NextRow(result)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "district not found"));
            }
            const int nextId = result.GetInt32(0);
            const auto tax = RateFromDouble(result.GetDouble(1));
            Session_.ExecuteModify(
                "UPDATE district SET d_next_o_id = $1 WHERE d_w_id = $2 AND d_id = $3",
                nextId + 1, p->WarehouseID, p->DistrictID).Get();
            TDistrictOrderReservation res;
            res.NextOrderID = nextId;
            res.DistrictTax = tax;
            return ReadyOp(OkOp(1, 1, res));
        }
        if (const auto* p = std::get_if<TGetCustomerById>(&op)) {
            auto result = Session_.ExecuteQuery(
                "SELECT c_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, "
                "c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment, "
                "c_payment_cnt, c_delivery_cnt, c_data, c_since "
                "FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3",
                p->WarehouseID, p->DistrictID, p->CustomerID).Get();
            if (!NextRow(result)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "customer not found"));
            }
            TCustomerRow cust;
            cust.CustomerID = result.GetInt32(0);
            cust.First = result.GetString(1);
            cust.Middle = result.GetString(2);
            cust.Last = result.GetString(3);
            cust.Street1 = result.GetString(4);
            cust.Street2 = result.GetString(5);
            cust.City = result.GetString(6);
            cust.State = result.GetString(7);
            cust.Zip = result.GetString(8);
            cust.Phone = result.GetString(9);
            cust.Credit = result.GetString(10);
            cust.CreditLimit = MoneyFromDouble(result.GetDouble(11));
            cust.Discount = RateFromDouble(result.GetDouble(12));
            cust.Balance = MoneyFromDouble(result.GetDouble(13));
            cust.YtdPayment = MoneyFromDouble(result.GetDouble(14));
            cust.PaymentCount = result.GetInt32(15);
            cust.DeliveryCount = result.GetInt32(16);
            cust.Data = result.GetString(17);
            cust.Since = result.GetString(18);
            return ReadyOp(OkOp(1, 1, std::move(cust)));
        }
        if (const auto* p = std::get_if<TGetItems>(&op)) {
            std::vector<TItemRow> items;
            items.reserve(p->ItemIDs.size());
            for (int id : p->ItemIDs) {
                auto result = Session_.ExecuteQuery(
                    "SELECT i_id, i_price, i_name, i_data FROM item WHERE i_id = $1", id).Get();
                if (!NextRow(result)) {
                    return ReadyOp(FailOp(EErrorClass::Integrity, "item not found"));
                }
                TItemRow item;
                item.ItemID = result.GetInt32(0);
                item.Price = MoneyFromDouble(result.GetDouble(1));
                item.Name = result.GetString(2);
                item.Data = result.GetString(3);
                items.push_back(std::move(item));
            }
            return ReadyOp(OkOp(p->ItemIDs.size(), items.size(), std::move(items)));
        }
        if (const auto* p = std::get_if<TCreateOrder>(&op)) {
            Session_.ExecuteModify(
                "INSERT INTO oorder (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) "
                "VALUES ($1,$2,$3,$4,CURRENT_TIMESTAMP,$5,$6)",
                p->OrderID, p->DistrictID, p->WarehouseID, p->CustomerID, p->LineCount, p->AllLocal).Get();
            Session_.ExecuteModify(
                "INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES ($1,$2,$3)",
                p->OrderID, p->DistrictID, p->WarehouseID).Get();
            return ReadyOp(OkOp(2, 2));
        }
        if (const auto* p = std::get_if<TUpdateStock>(&op)) {
            Session_.ExecuteModify(
                "UPDATE stock SET s_quantity=$1, s_ytd=s_ytd+$2, s_order_cnt=s_order_cnt+1, "
                "s_remote_cnt=s_remote_cnt+$3 WHERE s_w_id=$4 AND s_i_id=$5",
                p->NewQuantity, p->OrderedQuantity, p->RemoteIncrement,
                p->WarehouseID, p->ItemID).Get();
            return ReadyOp(OkOp(1, 1));
        }
        if (const auto* p = std::get_if<TInsertOrderLine>(&op)) {
            Session_.ExecuteModify(
                "INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, "
                "ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)",
                p->OrderID, p->DistrictID, p->WarehouseID, p->LineNumber, p->ItemID,
                p->SupplyWarehouseID, p->Quantity, p->Amount.ToString(), p->DistInfo).Get();
            return ReadyOp(OkOp(1, 1));
        }
        if (const auto* p = std::get_if<TCountRecentLowStock>(&op)) {
            auto dist = Session_.ExecuteQuery(
                "SELECT d_next_o_id FROM district WHERE d_w_id=$1 AND d_id=$2",
                p->WarehouseID, p->DistrictID).Get();
            if (!NextRow(dist)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "district not found"));
            }
            const int nextOid = dist.GetInt32(0);
            auto stock = Session_.ExecuteQuery(
                "SELECT COUNT(DISTINCT s.s_i_id) FROM order_line ol, stock s "
                "WHERE ol.ol_w_id=$1 AND ol.ol_d_id=$2 AND ol.ol_o_id < $3 AND "
                "ol.ol_o_id >= $4 AND s.s_w_id=$1 AND s.s_i_id=ol.ol_i_id AND s.s_quantity < $5",
                p->WarehouseID, p->DistrictID, nextOid,
                nextOid - p->RecentOrderCount, p->Threshold).Get();
            int count = 0;
            if (NextRow(stock)) {
                count = stock.GetInt32(0);
            }
            return ReadyOp(OkOp(1, 1, count));
        }
        if (const auto* p = std::get_if<TGetOldestNewOrder>(&op)) {
            auto result = Session_.ExecuteQuery(
                "SELECT no_o_id FROM new_order WHERE no_w_id=$1 AND no_d_id=$2 "
                "ORDER BY no_o_id ASC LIMIT 1",
                p->WarehouseID, p->DistrictID).Get();
            if (!NextRow(result)) {
                return ReadyOp(OkOp(0, 0, 0));
            }
            return ReadyOp(OkOp(1, 1, result.GetInt32(0)));
        }
        if (const auto* p = std::get_if<TApplyPaymentToLocation>(&op)) {
            Session_.ExecuteModify(
                "UPDATE warehouse SET w_ytd = w_ytd + $1 WHERE w_id = $2",
                p->Amount.ToString(), p->WarehouseID).Get();
            Session_.ExecuteModify(
                "UPDATE district SET d_ytd = d_ytd + $1 WHERE d_w_id = $2 AND d_id = $3",
                p->Amount.ToString(), p->WarehouseID, p->DistrictID).Get();
            auto wh = Session_.ExecuteQuery(
                "SELECT w_name, w_street_1, w_street_2, w_city, w_state, w_zip "
                "FROM warehouse WHERE w_id=$1", p->WarehouseID).Get();
            auto dist = Session_.ExecuteQuery(
                "SELECT d_name, d_street_1, d_street_2, d_city, d_state, d_zip "
                "FROM district WHERE d_w_id=$1 AND d_id=$2",
                p->WarehouseID, p->DistrictID).Get();
            TWarehouseDistrictInfo info;
            if (NextRow(wh)) {
                info.WarehouseName = wh.GetString(0);
                info.WarehouseStreet1 = wh.GetString(1);
                info.WarehouseStreet2 = wh.GetString(2);
                info.WarehouseCity = wh.GetString(3);
                info.WarehouseState = wh.GetString(4);
                info.WarehouseZip = wh.GetString(5);
            }
            if (NextRow(dist)) {
                info.DistrictName = dist.GetString(0);
                info.DistrictStreet1 = dist.GetString(1);
                info.DistrictStreet2 = dist.GetString(2);
                info.DistrictCity = dist.GetString(3);
                info.DistrictState = dist.GetString(4);
                info.DistrictZip = dist.GetString(5);
            }
            return ReadyOp(OkOp(2, 2, std::move(info)));
        }
        if (const auto* p = std::get_if<TInsertPaymentHistory>(&op)) {
            Session_.ExecuteModify(
                "INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data) "
                "VALUES ($1,$2,$3,$4,$5,CURRENT_TIMESTAMP,$6,$7)",
                p->CustomerID, p->CustomerDistrictID, p->CustomerWarehouseID,
                p->PaymentDistrictID, p->PaymentWarehouseID,
                p->Amount.ToString(), p->Data).Get();
            return ReadyOp(OkOp(1, 1));
        }
        if (const auto* p = std::get_if<TGetCustomersByLastName>(&op)) {
            auto result = Session_.ExecuteQuery(
                "SELECT c_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, "
                "c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment, "
                "c_payment_cnt, c_delivery_cnt, c_data, c_since "
                "FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_last = $3 "
                "ORDER BY c_first",
                p->WarehouseID, p->DistrictID, p->LastName).Get();
            std::vector<TCustomerRow> customers;
            while (NextRow(result)) {
                TCustomerRow cust;
                cust.CustomerID = result.GetInt32(0);
                cust.First = result.GetString(1);
                cust.Middle = result.GetString(2);
                cust.Last = result.GetString(3);
                cust.Street1 = result.GetString(4);
                cust.Street2 = result.GetString(5);
                cust.City = result.GetString(6);
                cust.State = result.GetString(7);
                cust.Zip = result.GetString(8);
                cust.Phone = result.GetString(9);
                cust.Credit = result.GetString(10);
                cust.CreditLimit = MoneyFromDouble(result.GetDouble(11));
                cust.Discount = RateFromDouble(result.GetDouble(12));
                cust.Balance = MoneyFromDouble(result.GetDouble(13));
                cust.YtdPayment = MoneyFromDouble(result.GetDouble(14));
                cust.PaymentCount = result.GetInt32(15);
                cust.DeliveryCount = result.GetInt32(16);
                cust.Data = result.GetString(17);
                cust.Since = result.GetString(18);
                customers.push_back(std::move(cust));
            }
            return ReadyOp(OkOp(customers.size(), customers.size(), std::move(customers)));
        }
        if (const auto* p = std::get_if<TGetStocksForUpdate>(&op)) {
            std::vector<TStockRow> stocks;
            stocks.reserve(p->Stocks.size());
            for (const auto& key : p->Stocks) {
                auto result = Session_.ExecuteQuery(
                    "SELECT s_quantity, s_ytd, s_order_cnt, s_remote_cnt, s_data, "
                    "s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, "
                    "s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 "
                    "FROM stock WHERE s_w_id = $1 AND s_i_id = $2 FOR UPDATE",
                    key.WarehouseID, key.ItemID).Get();
                if (!NextRow(result)) {
                    return ReadyOp(FailOp(EErrorClass::Integrity, "stock not found"));
                }
                TStockRow row;
                row.WarehouseID = key.WarehouseID;
                row.ItemID = key.ItemID;
                row.Quantity = result.GetInt32(0);
                row.Ytd = MoneyFromDouble(result.GetDouble(1));
                row.OrderCount = result.GetInt32(2);
                row.RemoteCount = result.GetInt32(3);
                row.Data = result.GetString(4);
                const int distIdx = p->DistrictID;
                if (distIdx >= 1 && distIdx <= 10) {
                    row.DistInfo = result.GetString(static_cast<size_t>(4 + distIdx));
                }
                stocks.push_back(std::move(row));
            }
            return ReadyOp(OkOp(p->Stocks.size(), stocks.size(), std::move(stocks)));
        }
        if (const auto* p = std::get_if<TGetCustomerData>(&op)) {
            auto result = Session_.ExecuteQuery(
                "SELECT c_data FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3",
                p->WarehouseID, p->DistrictID, p->CustomerID).Get();
            if (!NextRow(result)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "customer not found"));
            }
            return ReadyOp(OkOp(1, 1, result.GetString(0)));
        }
        if (const auto* p = std::get_if<TUpdateCustomerPayment>(&op)) {
            if (p->UpdateData) {
                Session_.ExecuteModify(
                    "UPDATE customer SET c_balance = $1, c_ytd_payment = $2, c_payment_cnt = $3, c_data = $4 "
                    "WHERE c_w_id = $5 AND c_d_id = $6 AND c_id = $7",
                    p->NewBalance.ToString(), p->NewYtdPayment.ToString(), p->NewPaymentCount,
                    p->NewData, p->WarehouseID, p->DistrictID, p->CustomerID).Get();
            } else {
                Session_.ExecuteModify(
                    "UPDATE customer SET c_balance = $1, c_ytd_payment = $2, c_payment_cnt = $3 "
                    "WHERE c_w_id = $4 AND c_d_id = $5 AND c_id = $6",
                    p->NewBalance.ToString(), p->NewYtdPayment.ToString(), p->NewPaymentCount,
                    p->WarehouseID, p->DistrictID, p->CustomerID).Get();
            }
            return ReadyOp(OkOp(1, 1));
        }
        if (const auto* p = std::get_if<TGetLatestCustomerOrder>(&op)) {
            auto result = Session_.ExecuteQuery(
                "SELECT o_id, o_c_id, o_carrier_id, o_entry_d FROM oorder "
                "WHERE o_w_id = $1 AND o_d_id = $2 AND o_c_id = $3 "
                "ORDER BY o_id DESC LIMIT 1",
                p->WarehouseID, p->DistrictID, p->CustomerID).Get();
            if (!NextRow(result)) {
                return ReadyOp(OkOp(0, 0));
            }
            TOrderHeader header;
            header.OrderID = result.GetInt32(0);
            header.CustomerID = result.GetInt32(1);
            // o_carrier_id may be null
            auto carrierField = result.GetRawResult()[0][2];
            if (!carrierField.is_null()) {
                header.CarrierID = carrierField.as<int>();
            }
            header.EntryDate = result.GetString(3);
            return ReadyOp(OkOp(1, 1, std::move(header)));
        }
        if (const auto* p = std::get_if<TGetOrderStatusLines>(&op)) {
            auto result = Session_.ExecuteQuery(
                "SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d "
                "FROM order_line WHERE ol_w_id = $1 AND ol_d_id = $2 AND ol_o_id = $3",
                p->WarehouseID, p->DistrictID, p->OrderID).Get();
            std::vector<TOrderStatusLine> lines;
            size_t rowIdx = 0;
            while (NextRow(result)) {
                TOrderStatusLine line;
                line.ItemID = result.GetInt32(0);
                line.SupplyWarehouseID = result.GetInt32(1);
                line.Quantity = result.GetInt32(2);
                line.Amount = MoneyFromDouble(result.GetDouble(3));
                auto deliv = result.GetRawResult()[rowIdx][4];
                if (!deliv.is_null()) {
                    line.DeliveryDate = deliv.as<std::string>();
                }
                lines.push_back(std::move(line));
                ++rowIdx;
            }
            return ReadyOp(OkOp(lines.size(), lines.size(), std::move(lines)));
        }
        if (const auto* p = std::get_if<TGetDeliveryOrderInfo>(&op)) {
            auto cid = Session_.ExecuteQuery(
                "SELECT o_c_id FROM oorder WHERE o_w_id = $1 AND o_d_id = $2 AND o_id = $3",
                p->WarehouseID, p->DistrictID, p->OrderID).Get();
            if (!NextRow(cid)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "order not found"));
            }
            TDeliveryOrderInfo info;
            info.CustomerID = cid.GetInt32(0);
            auto ol = Session_.ExecuteQuery(
                "SELECT ol_amount FROM order_line "
                "WHERE ol_w_id = $1 AND ol_d_id = $2 AND ol_o_id = $3",
                p->WarehouseID, p->DistrictID, p->OrderID).Get();
            int64_t totalCents = 0;
            while (NextRow(ol)) {
                totalCents += MoneyFromDouble(ol.GetDouble(0)).Cents();
                ++info.LineCount;
            }
            info.TotalAmount = TMoney::FromCents(totalCents);
            return ReadyOp(OkOp(1, 1, std::move(info)));
        }
        if (const auto* p = std::get_if<TCompleteOrderDelivery>(&op)) {
            Session_.ExecuteModify(
                "DELETE FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 AND no_o_id = $3",
                p->WarehouseID, p->DistrictID, p->OrderID).Get();
            Session_.ExecuteModify(
                "UPDATE oorder SET o_carrier_id = $1 WHERE o_w_id = $2 AND o_d_id = $3 AND o_id = $4",
                p->CarrierID, p->WarehouseID, p->DistrictID, p->OrderID).Get();
            Session_.ExecuteModify(
                "UPDATE order_line SET ol_delivery_d = CURRENT_TIMESTAMP "
                "WHERE ol_w_id = $1 AND ol_d_id = $2 AND ol_o_id = $3",
                p->WarehouseID, p->DistrictID, p->OrderID).Get();
            return ReadyOp(OkOp(3, 3));
        }
        if (const auto* p = std::get_if<TApplyDeliveryToCustomer>(&op)) {
            Session_.ExecuteModify(
                "UPDATE customer SET c_balance = c_balance + $1, c_delivery_cnt = c_delivery_cnt + 1 "
                "WHERE c_w_id = $2 AND c_d_id = $3 AND c_id = $4",
                p->Amount.ToString(), p->WarehouseID, p->DistrictID, p->CustomerID).Get();
            return ReadyOp(OkOp(1, 1));
        }

        return ReadyOp(FailOp(
            EErrorClass::Permanent,
            "semantic op not yet bound in TPgTpccTransaction"));
    } catch (const std::exception& ex) {
        return ReadyOp(FailOp(Classifier_.ClassifyException(ex), ex.what(), PgSqlStateOf(ex)));
    }
}

TPgTpccSession::TPgTpccSession(PgSession& session)
    : Session_(session)
{}

TFuture<std::unique_ptr<ITpccTransaction>> TPgTpccSession::Begin(EIsolationLevel /*isolation*/) {
    // PgSession uses fixed repeatable_read; requested level is recorded via capabilities.
    TPromise<std::unique_ptr<ITpccTransaction>> promise;
    auto future = promise.GetFuture();
    promise.SetValue(std::make_unique<TPgTpccTransaction>(Session_));
    return future;
}

namespace {

class TPgOwnedTpccSession : public ITpccSession {
public:
    explicit TPgOwnedTpccSession(PgConnectionPool::SessionGuard guard)
        : Guard_(std::move(guard))
        , Inner_(*Guard_)
    {}

    TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel isolation) override {
        return Inner_.Begin(isolation);
    }

private:
    PgConnectionPool::SessionGuard Guard_;
    TPgTpccSession Inner_;
};

} // namespace

TPgSessionFactory::TPgSessionFactory(PgConnectionPool& pool)
    : Pool_(pool)
{}

std::unique_ptr<ITpccSession> TPgSessionFactory::CreateSession() {
    return std::make_unique<TPgOwnedTpccSession>(Pool_.AcquireGuard());
}

} // namespace NTpcc

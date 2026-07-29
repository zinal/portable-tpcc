#include "tpcc_session.h"

#include "query_result.h"

#include <coro_traits.h>
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

} // namespace

TPgTpccTransaction::TPgTpccTransaction(PgSession& session)
    : Session_(session)
{}

bool TPgTpccTransaction::TerminalState() const {
    return Terminal_;
}

TFuture<TCommitResult> TPgTpccTransaction::Commit() {
    if (Terminal_) {
        co_return TCommitResult{
            ECommitOutcome::OutcomeUnknown,
            EErrorClass::Permanent,
            {},
            "Commit called in terminal state"};
    }
    try {
        co_await Session_.Commit();
        Terminal_ = true;
        co_return TCommitResult{ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}};
    } catch (const std::exception& ex) {
        Terminal_ = true;
        const auto cls = Classifier_.ClassifyCommitException(ex);
        co_return TCommitResult{
            cls == EErrorClass::AmbiguousCommit ? ECommitOutcome::OutcomeUnknown
                                                : ECommitOutcome::RolledBack,
            cls,
            PgSqlStateOf(ex),
            ex.what()};
    }
}

TFuture<TCommitResult> TPgTpccTransaction::Rollback() {
    if (Terminal_) {
        co_return TCommitResult{
            ECommitOutcome::RolledBack,
            EErrorClass::Permanent,
            {},
            "Rollback called in terminal state"};
    }
    try {
        co_await Session_.Rollback();
        Terminal_ = true;
        co_return TCommitResult{ECommitOutcome::RolledBack, EErrorClass::Permanent, {}, {}};
    } catch (const std::exception& ex) {
        Terminal_ = true;
        co_return TCommitResult{
            ECommitOutcome::RolledBack,
            Classifier_.ClassifyException(ex),
            PgSqlStateOf(ex),
            ex.what()};
    }
}

TFuture<TCommitResult> TPgTpccTransaction::Cancel() {
    // Best-effort rollback; PgSession cancellation is via shutdown flag on the pool.
    auto result = co_await Rollback();
    result.ErrorClass = EErrorClass::Cancelled;
    co_return result;
}

TFuture<TBatchResult> TPgTpccTransaction::ExecuteBatch(const std::vector<TSemanticOp>& ops) {
    TBatchResult batch;
    batch.Ok = true;
    for (const auto& op : ops) {
        auto one = co_await Execute(op);
        batch.Results.push_back(one);
        if (!one.Ok) {
            batch.Ok = false;
            batch.ErrorClass = one.ErrorClass;
            batch.NativeCode = one.NativeCode;
            batch.Message = one.Message;
            break;
        }
    }
    co_return batch;
}

TFuture<TFinalCommitResult> TPgTpccTransaction::ExecuteFinalAndCommit(const TSemanticOp& op) {
    TFinalCommitResult out;
    out.Operation = co_await Execute(op);
    if (!out.Operation.Ok) {
        out.Commit = co_await Rollback();
        co_return out;
    }
    out.Commit = co_await Commit();
    co_return out;
}

TFuture<TOperationResult> TPgTpccTransaction::Execute(const TSemanticOp& op) {
    if (Terminal_) {
        co_return FailOp(EErrorClass::Permanent, "Execute called in terminal state");
    }
    try {
        if (const auto* p = std::get_if<TGetWarehouseTax>(&op)) {
            auto result = co_await Session_.ExecuteQuery(
                "SELECT w_tax FROM warehouse WHERE w_id = $1", p->WarehouseID);
            if (!NextRow(result)) {
                co_return FailOp(EErrorClass::Integrity, "warehouse not found", {});
            }
            co_return OkOp(1, 1, RateFromDouble(result.GetDouble(0)));
        }
        if (const auto* p = std::get_if<TReserveDistrictOrderId>(&op)) {
            auto result = co_await Session_.ExecuteQuery(
                "SELECT d_next_o_id, d_tax FROM district "
                "WHERE d_w_id = $1 AND d_id = $2 FOR UPDATE",
                p->WarehouseID, p->DistrictID);
            if (!NextRow(result)) {
                co_return FailOp(EErrorClass::Integrity, "district not found");
            }
            const int nextId = result.GetInt32(0);
            const auto tax = RateFromDouble(result.GetDouble(1));
            co_await Session_.ExecuteModify(
                "UPDATE district SET d_next_o_id = $1 WHERE d_w_id = $2 AND d_id = $3",
                nextId + 1, p->WarehouseID, p->DistrictID);
            TDistrictOrderReservation res;
            res.NextOrderID = nextId;
            res.DistrictTax = tax;
            co_return OkOp(1, 1, res);
        }
        if (const auto* p = std::get_if<TGetCustomerById>(&op)) {
            auto result = co_await Session_.ExecuteQuery(
                "SELECT c_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, "
                "c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment, "
                "c_payment_cnt, c_delivery_cnt, c_data, c_since "
                "FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3",
                p->WarehouseID, p->DistrictID, p->CustomerID);
            if (!NextRow(result)) {
                co_return FailOp(EErrorClass::Integrity, "customer not found");
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
            co_return OkOp(1, 1, std::move(cust));
        }
        if (const auto* p = std::get_if<TGetItems>(&op)) {
            std::vector<TItemRow> items;
            items.reserve(p->ItemIDs.size());
            for (int id : p->ItemIDs) {
                auto result = co_await Session_.ExecuteQuery(
                    "SELECT i_id, i_price, i_name, i_data FROM item WHERE i_id = $1", id);
                if (!NextRow(result)) {
                    co_return FailOp(EErrorClass::Integrity, "item not found");
                }
                TItemRow item;
                item.ItemID = result.GetInt32(0);
                item.Price = MoneyFromDouble(result.GetDouble(1));
                item.Name = result.GetString(2);
                item.Data = result.GetString(3);
                items.push_back(std::move(item));
            }
            co_return OkOp(p->ItemIDs.size(), items.size(), std::move(items));
        }
        if (const auto* p = std::get_if<TCreateOrder>(&op)) {
            co_await Session_.ExecuteModify(
                "INSERT INTO oorder (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) "
                "VALUES ($1,$2,$3,$4,CURRENT_TIMESTAMP,$5,$6)",
                p->OrderID, p->DistrictID, p->WarehouseID, p->CustomerID, p->LineCount, p->AllLocal);
            co_await Session_.ExecuteModify(
                "INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES ($1,$2,$3)",
                p->OrderID, p->DistrictID, p->WarehouseID);
            co_return OkOp(2, 2);
        }
        if (const auto* p = std::get_if<TUpdateStock>(&op)) {
            co_await Session_.ExecuteModify(
                "UPDATE stock SET s_quantity=$1, s_ytd=s_ytd+$2, s_order_cnt=s_order_cnt+1, "
                "s_remote_cnt=s_remote_cnt+$3 WHERE s_w_id=$4 AND s_i_id=$5",
                p->NewQuantity, p->OrderedQuantity, p->RemoteIncrement,
                p->WarehouseID, p->ItemID);
            co_return OkOp(1, 1);
        }
        if (const auto* p = std::get_if<TInsertOrderLine>(&op)) {
            co_await Session_.ExecuteModify(
                "INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, "
                "ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)",
                p->OrderID, p->DistrictID, p->WarehouseID, p->LineNumber, p->ItemID,
                p->SupplyWarehouseID, p->Quantity, p->Amount.ToString(), p->DistInfo);
            co_return OkOp(1, 1);
        }
        if (const auto* p = std::get_if<TCountRecentLowStock>(&op)) {
            auto dist = co_await Session_.ExecuteQuery(
                "SELECT d_next_o_id FROM district WHERE d_w_id=$1 AND d_id=$2",
                p->WarehouseID, p->DistrictID);
            if (!NextRow(dist)) {
                co_return FailOp(EErrorClass::Integrity, "district not found");
            }
            const int nextOid = dist.GetInt32(0);
            auto stock = co_await Session_.ExecuteQuery(
                "SELECT COUNT(DISTINCT s.s_i_id) FROM order_line ol, stock s "
                "WHERE ol.ol_w_id=$1 AND ol.ol_d_id=$2 AND ol.ol_o_id < $3 AND "
                "ol.ol_o_id >= $4 AND s.s_w_id=$1 AND s.s_i_id=ol.ol_i_id AND s.s_quantity < $5",
                p->WarehouseID, p->DistrictID, nextOid,
                nextOid - p->RecentOrderCount, p->Threshold);
            int count = 0;
            if (NextRow(stock)) {
                count = stock.GetInt32(0);
            }
            co_return OkOp(1, 1, count);
        }
        if (const auto* p = std::get_if<TGetOldestNewOrder>(&op)) {
            auto result = co_await Session_.ExecuteQuery(
                "SELECT no_o_id FROM new_order WHERE no_w_id=$1 AND no_d_id=$2 "
                "ORDER BY no_o_id ASC LIMIT 1",
                p->WarehouseID, p->DistrictID);
            if (!NextRow(result)) {
                co_return OkOp(0, 0, 0);
            }
            co_return OkOp(1, 1, result.GetInt32(0));
        }
        if (const auto* p = std::get_if<TApplyPaymentToLocation>(&op)) {
            co_await Session_.ExecuteModify(
                "UPDATE warehouse SET w_ytd = w_ytd + $1 WHERE w_id = $2",
                p->Amount.ToString(), p->WarehouseID);
            co_await Session_.ExecuteModify(
                "UPDATE district SET d_ytd = d_ytd + $1 WHERE d_w_id = $2 AND d_id = $3",
                p->Amount.ToString(), p->WarehouseID, p->DistrictID);
            auto wh = co_await Session_.ExecuteQuery(
                "SELECT w_name, w_street_1, w_street_2, w_city, w_state, w_zip "
                "FROM warehouse WHERE w_id=$1", p->WarehouseID);
            auto dist = co_await Session_.ExecuteQuery(
                "SELECT d_name, d_street_1, d_street_2, d_city, d_state, d_zip "
                "FROM district WHERE d_w_id=$1 AND d_id=$2",
                p->WarehouseID, p->DistrictID);
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
            co_return OkOp(2, 2, std::move(info));
        }
        if (const auto* p = std::get_if<TInsertPaymentHistory>(&op)) {
            co_await Session_.ExecuteModify(
                "INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data) "
                "VALUES ($1,$2,$3,$4,$5,CURRENT_TIMESTAMP,$6,$7)",
                p->CustomerID, p->CustomerDistrictID, p->CustomerWarehouseID,
                p->PaymentDistrictID, p->PaymentWarehouseID,
                p->Amount.ToString(), p->Data);
            co_return OkOp(1, 1);
        }

        co_return FailOp(
            EErrorClass::Permanent,
            "semantic op not yet bound in TPgTpccTransaction");
    } catch (const std::exception& ex) {
        co_return FailOp(Classifier_.ClassifyException(ex), ex.what(), PgSqlStateOf(ex));
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
        , Inner_(**Guard_)
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

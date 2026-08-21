#include "tpcc_session.h"

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

TOperationResult CheckAffected(uint64_t actual, uint64_t expected, const std::string& what) {
    if (actual != expected) {
        return FailOp(
            EErrorClass::Integrity,
            what + ": affected " + std::to_string(actual) +
                " rows, expected " + std::to_string(expected));
    }
    return OkOp(expected, actual);
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

TCustomerRow ReadCustomer(QueryResult& result) {
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
    cust.CreditLimit = result.GetMoney(11);
    cust.Discount = result.GetRate(12);
    cust.Balance = result.GetMoney(13);
    cust.YtdPayment = result.GetMoney(14);
    cust.PaymentCount = result.GetInt32(15);
    cust.DeliveryCount = result.GetInt32(16);
    cust.Data = result.GetString(17);
    cust.Since = result.GetString(18);
    return cust;
}

} // namespace

TObTpccTransaction::TObTpccTransaction(TObSession& session)
    : Session_(session)
{}

TFuture<TOperationResult> TObTpccTransaction::ExecuteSelect1() {
    if (Terminal_) {
        return ReadyOp(FailOp(EErrorClass::Permanent, "ExecuteSelect1 called in terminal state"));
    }
    try {
        Session_.ExecuteQuery(EObQueryId::SimulationSelectCastInt, MakeParams(1)).Get();
        return ReadyOp(OkOp(1, 1));
    } catch (const std::exception& ex) {
        return ReadyOp(FailOp(Classifier_.ClassifyException(ex), ex.what(), ObNativeCodeOf(ex)));
    }
}

TFuture<TCommitResult> TObTpccTransaction::Commit() {
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
            ObNativeCodeOf(ex),
            ex.what()});
    }
}

TFuture<TCommitResult> TObTpccTransaction::Rollback() {
    if (Terminal_) {
        return ReadyCommit({
            ECommitOutcome::OutcomeUnknown,
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
            ECommitOutcome::OutcomeUnknown,
            Classifier_.ClassifyException(ex),
            ObNativeCodeOf(ex),
            ex.what()});
    }
}

TFuture<TCommitResult> TObTpccTransaction::Cancel() {
    auto result = Rollback().Get();
    result.ErrorClass = EErrorClass::Cancelled;
    return ReadyCommit(std::move(result));
}

TFuture<TBatchResult> TObTpccTransaction::ExecuteBatch(const std::vector<TSemanticOp>& ops) {
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

TFuture<TFinalCommitResult> TObTpccTransaction::ExecuteFinalAndCommit(const TSemanticOp& op) {
    TFinalCommitResult out;
    out.Operation = Execute(op).Get();
    if (!out.Operation.Ok) {
        out.Commit = Rollback().Get();
        return ReadyFinal(std::move(out));
    }
    out.Commit = Commit().Get();
    return ReadyFinal(std::move(out));
}

TFuture<TOperationResult> TObTpccTransaction::Execute(const TSemanticOp& op) {
    if (Terminal_) {
        return ReadyOp(FailOp(EErrorClass::Permanent, "Execute called in terminal state"));
    }
    try {
        if (const auto* p = std::get_if<TGetWarehouseTax>(&op)) {
            auto result = Session_.ExecuteQuery(
                EObQueryId::GetWarehouseTax,
                MakeParams(p->WarehouseID)).Get();
            if (!NextRow(result)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "warehouse not found"));
            }
            return ReadyOp(OkOp(1, 1, result.GetRate(0)));
        }
        if (const auto* p = std::get_if<TReserveDistrictOrderId>(&op)) {
            auto result = Session_.ExecuteQuery(
                EObQueryId::ReserveDistrictOrderId,
                MakeParams(p->WarehouseID, p->DistrictID)).Get();
            if (!NextRow(result)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "district not found"));
            }
            const int nextId = result.GetInt32(0);
            const auto tax = result.GetRate(1);
            auto affected = Session_.ExecuteModify(
                EObQueryId::UpdateDistrictNextOrderId,
                MakeParams(nextId + 1, p->WarehouseID, p->DistrictID)).Get();
            auto check = CheckAffected(affected, 1, "district next order update");
            if (!check.Ok) return ReadyOp(std::move(check));
            TDistrictOrderReservation res;
            res.NextOrderID = nextId;
            res.DistrictTax = tax;
            return ReadyOp(OkOp(1, 1, res));
        }
        if (const auto* p = std::get_if<TGetCustomerById>(&op)) {
            auto result = Session_.ExecuteQuery(
                EObQueryId::GetCustomerById,
                MakeParams(p->WarehouseID, p->DistrictID, p->CustomerID)).Get();
            if (!NextRow(result)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "customer not found"));
            }
            return ReadyOp(OkOp(1, 1, ReadCustomer(result)));
        }
        if (const auto* p = std::get_if<TGetItems>(&op)) {
            std::vector<TItemRow> items;
            items.reserve(p->ItemIDs.size());
            for (int id : p->ItemIDs) {
                auto result = Session_.ExecuteQuery(EObQueryId::GetItems, MakeParams(id)).Get();
                if (!NextRow(result)) {
                    return ReadyOp(FailOp(EErrorClass::Integrity, "item not found"));
                }
                TItemRow item;
                item.ItemID = result.GetInt32(0);
                item.Price = result.GetMoney(1);
                item.Name = result.GetString(2);
                item.Data = result.GetString(3);
                items.push_back(std::move(item));
            }
            return ReadyOp(OkOp(p->ItemIDs.size(), items.size(), std::move(items)));
        }
        if (const auto* p = std::get_if<TCreateOrder>(&op)) {
            auto orderAffected = Session_.ExecuteModify(
                EObQueryId::CreateOrder,
                MakeParams(p->OrderID, p->DistrictID, p->WarehouseID, p->CustomerID,
                           p->LineCount, p->AllLocal)).Get();
            auto orderCheck = CheckAffected(orderAffected, 1, "oorder insert");
            if (!orderCheck.Ok) return ReadyOp(std::move(orderCheck));
            auto newOrderAffected = Session_.ExecuteModify(
                EObQueryId::CreateNewOrder,
                MakeParams(p->OrderID, p->DistrictID, p->WarehouseID)).Get();
            auto newOrderCheck = CheckAffected(newOrderAffected, 1, "new_order insert");
            if (!newOrderCheck.Ok) return ReadyOp(std::move(newOrderCheck));
            return ReadyOp(OkOp(2, 2));
        }
        if (const auto* p = std::get_if<TUpdateStock>(&op)) {
            auto affected = Session_.ExecuteModify(
                EObQueryId::UpdateStock,
                MakeParams(p->NewQuantity, p->OrderedQuantity, p->RemoteIncrement,
                           p->WarehouseID, p->ItemID)).Get();
            auto check = CheckAffected(affected, 1, "stock update");
            if (!check.Ok) return ReadyOp(std::move(check));
            return ReadyOp(OkOp(1, 1));
        }
        if (const auto* p = std::get_if<TInsertOrderLine>(&op)) {
            auto affected = Session_.ExecuteModify(
                EObQueryId::InsertOrderLine,
                MakeParams(p->OrderID, p->DistrictID, p->WarehouseID, p->LineNumber,
                           p->ItemID, p->SupplyWarehouseID, p->Quantity,
                           p->Amount.ToString(), p->DistInfo)).Get();
            auto check = CheckAffected(affected, 1, "order_line insert");
            if (!check.Ok) return ReadyOp(std::move(check));
            return ReadyOp(OkOp(1, 1));
        }
        if (const auto* p = std::get_if<TCountRecentLowStock>(&op)) {
            auto dist = Session_.ExecuteQuery(
                EObQueryId::CountRecentDistrict,
                MakeParams(p->WarehouseID, p->DistrictID)).Get();
            if (!NextRow(dist)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "district not found"));
            }
            const int nextOid = dist.GetInt32(0);
            auto stock = Session_.ExecuteQuery(
                EObQueryId::CountRecentLowStock,
                MakeParams(p->WarehouseID, p->DistrictID, nextOid,
                           nextOid - p->RecentOrderCount, p->WarehouseID, p->Threshold)).Get();
            int count = 0;
            if (NextRow(stock)) {
                count = stock.GetInt32(0);
            }
            return ReadyOp(OkOp(1, 1, count));
        }
        if (const auto* p = std::get_if<TGetOldestNewOrder>(&op)) {
            auto result = Session_.ExecuteQuery(
                EObQueryId::GetOldestNewOrder,
                MakeParams(p->WarehouseID, p->DistrictID)).Get();
            if (!NextRow(result)) {
                return ReadyOp(OkOp(0, 0, 0));
            }
            return ReadyOp(OkOp(1, 1, result.GetInt32(0)));
        }
        if (const auto* p = std::get_if<TApplyPaymentToLocation>(&op)) {
            auto whAffected = Session_.ExecuteModify(
                EObQueryId::ApplyPaymentWarehouse,
                MakeParams(p->Amount.ToString(), p->WarehouseID)).Get();
            auto check = CheckAffected(whAffected, 1, "warehouse payment update");
            if (!check.Ok) return ReadyOp(std::move(check));
            auto distAffected = Session_.ExecuteModify(
                EObQueryId::ApplyPaymentDistrict,
                MakeParams(p->Amount.ToString(), p->WarehouseID, p->DistrictID)).Get();
            check = CheckAffected(distAffected, 1, "district payment update");
            if (!check.Ok) return ReadyOp(std::move(check));
            auto wh = Session_.ExecuteQuery(
                EObQueryId::SelectPaymentWarehouse,
                MakeParams(p->WarehouseID)).Get();
            auto dist = Session_.ExecuteQuery(
                EObQueryId::SelectPaymentDistrict,
                MakeParams(p->WarehouseID, p->DistrictID)).Get();
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
            auto affected = Session_.ExecuteModify(
                EObQueryId::InsertPaymentHistory,
                MakeParams(p->CustomerID, p->CustomerDistrictID, p->CustomerWarehouseID,
                           p->PaymentDistrictID, p->PaymentWarehouseID,
                           p->Amount.ToString(), p->Data)).Get();
            auto check = CheckAffected(affected, 1, "history insert");
            if (!check.Ok) return ReadyOp(std::move(check));
            return ReadyOp(OkOp(1, 1));
        }
        if (const auto* p = std::get_if<TGetCustomersByLastName>(&op)) {
            auto result = Session_.ExecuteQuery(
                EObQueryId::GetCustomersByLastName,
                MakeParams(p->WarehouseID, p->DistrictID, p->LastName)).Get();
            std::vector<TCustomerRow> customers;
            while (NextRow(result)) {
                customers.push_back(ReadCustomer(result));
            }
            return ReadyOp(OkOp(customers.size(), customers.size(), std::move(customers)));
        }
        if (const auto* p = std::get_if<TGetStocksForUpdate>(&op)) {
            std::vector<TStockRow> stocks;
            stocks.reserve(p->Stocks.size());
            for (const auto& key : p->Stocks) {
                auto result = Session_.ExecuteQuery(
                    EObQueryId::GetStockForUpdate,
                    MakeParams(key.WarehouseID, key.ItemID)).Get();
                if (!NextRow(result)) {
                    return ReadyOp(FailOp(EErrorClass::Integrity, "stock not found"));
                }
                TStockRow row;
                row.WarehouseID = key.WarehouseID;
                row.ItemID = key.ItemID;
                row.Quantity = result.GetInt32(0);
                row.Ytd = result.GetMoney(1);
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
                EObQueryId::GetCustomerData,
                MakeParams(p->WarehouseID, p->DistrictID, p->CustomerID)).Get();
            if (!NextRow(result)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "customer not found"));
            }
            return ReadyOp(OkOp(1, 1, result.GetString(0)));
        }
        if (const auto* p = std::get_if<TUpdateCustomerPayment>(&op)) {
            uint64_t affected = 0;
            if (p->UpdateData) {
                affected = Session_.ExecuteModify(
                    EObQueryId::UpdateCustomerPaymentWithData,
                    MakeParams(p->NewBalance.ToString(), p->NewYtdPayment.ToString(),
                               p->NewPaymentCount, p->NewData,
                               p->WarehouseID, p->DistrictID, p->CustomerID)).Get();
            } else {
                affected = Session_.ExecuteModify(
                    EObQueryId::UpdateCustomerPayment,
                    MakeParams(p->NewBalance.ToString(), p->NewYtdPayment.ToString(),
                               p->NewPaymentCount,
                               p->WarehouseID, p->DistrictID, p->CustomerID)).Get();
            }
            auto check = CheckAffected(affected, 1, "customer payment update");
            if (!check.Ok) return ReadyOp(std::move(check));
            return ReadyOp(OkOp(1, 1));
        }
        if (const auto* p = std::get_if<TGetLatestCustomerOrder>(&op)) {
            auto result = Session_.ExecuteQuery(
                EObQueryId::GetLatestCustomerOrder,
                MakeParams(p->WarehouseID, p->DistrictID, p->CustomerID)).Get();
            if (!NextRow(result)) {
                return ReadyOp(OkOp(0, 0));
            }
            TOrderHeader header;
            header.OrderID = result.GetInt32(0);
            header.CustomerID = result.GetInt32(1);
            header.CarrierID = result.GetOptionalInt32(2);
            header.EntryDate = result.GetString(3);
            return ReadyOp(OkOp(1, 1, std::move(header)));
        }
        if (const auto* p = std::get_if<TGetOrderStatusLines>(&op)) {
            auto result = Session_.ExecuteQuery(
                EObQueryId::GetOrderStatusLines,
                MakeParams(p->WarehouseID, p->DistrictID, p->OrderID)).Get();
            std::vector<TOrderStatusLine> lines;
            while (NextRow(result)) {
                TOrderStatusLine line;
                line.ItemID = result.GetInt32(0);
                line.SupplyWarehouseID = result.GetInt32(1);
                line.Quantity = result.GetInt32(2);
                line.Amount = result.GetMoney(3);
                if (auto deliv = result.GetOptionalString(4)) {
                    line.DeliveryDate = *deliv;
                }
                lines.push_back(std::move(line));
            }
            return ReadyOp(OkOp(lines.size(), lines.size(), std::move(lines)));
        }
        if (const auto* p = std::get_if<TGetDeliveryOrderInfo>(&op)) {
            auto cid = Session_.ExecuteQuery(
                EObQueryId::GetDeliveryOrderCustomer,
                MakeParams(p->WarehouseID, p->DistrictID, p->OrderID)).Get();
            if (!NextRow(cid)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "order not found"));
            }
            TDeliveryOrderInfo info;
            info.CustomerID = cid.GetInt32(0);
            auto ol = Session_.ExecuteQuery(
                EObQueryId::GetDeliveryOrderLines,
                MakeParams(p->WarehouseID, p->DistrictID, p->OrderID)).Get();
            int64_t totalCents = 0;
            while (NextRow(ol)) {
                totalCents += ol.GetMoney(0).Cents();
                ++info.LineCount;
            }
            info.TotalAmount = TMoney::FromCents(totalCents);
            return ReadyOp(OkOp(1, 1, std::move(info)));
        }
        if (const auto* p = std::get_if<TCompleteOrderDelivery>(&op)) {
            const uint64_t deleted = Session_.ExecuteModify(
                EObQueryId::DeleteNewOrder,
                MakeParams(p->WarehouseID, p->DistrictID, p->OrderID)).Get();
            if (deleted == 0) {
                return ReadyOp(FailOp(
                    EErrorClass::RetryableAbort,
                    "new_order row already claimed by concurrent delivery"));
            }
            if (deleted != 1) {
                auto check = CheckAffected(deleted, 1, "new_order delivery delete");
                return ReadyOp(std::move(check));
            }
            auto orderAffected = Session_.ExecuteModify(
                EObQueryId::UpdateOrderCarrier,
                MakeParams(p->CarrierID, p->WarehouseID, p->DistrictID, p->OrderID)).Get();
            auto check = CheckAffected(orderAffected, 1, "order carrier update");
            if (!check.Ok) return ReadyOp(std::move(check));
            auto linesAffected = Session_.ExecuteModify(
                EObQueryId::UpdateOrderLineDelivery,
                MakeParams(p->WarehouseID, p->DistrictID, p->OrderID)).Get();
            check = CheckAffected(linesAffected, p->LineCount, "order_line delivery update");
            if (!check.Ok) return ReadyOp(std::move(check));
            return ReadyOp(OkOp(3, 3));
        }
        if (const auto* p = std::get_if<TApplyDeliveryToCustomer>(&op)) {
            auto affected = Session_.ExecuteModify(
                EObQueryId::ApplyDeliveryToCustomer,
                MakeParams(p->Amount.ToString(), p->WarehouseID, p->DistrictID,
                           p->CustomerID)).Get();
            auto check = CheckAffected(affected, 1, "customer delivery update");
            if (!check.Ok) return ReadyOp(std::move(check));
            return ReadyOp(OkOp(1, 1));
        }

        return ReadyOp(FailOp(EErrorClass::Permanent, "semantic op not bound in TObTpccTransaction"));
    } catch (const std::exception& ex) {
        return ReadyOp(FailOp(Classifier_.ClassifyException(ex), ex.what(), ObNativeCodeOf(ex)));
    }
}

TObTpccSession::TObTpccSession(TObSession& session)
    : Session_(session)
{}

TFuture<std::unique_ptr<ITpccTransaction>> TObTpccSession::Begin(EIsolationLevel /*isolation*/) {
    TPromise<std::unique_ptr<ITpccTransaction>> promise;
    auto future = promise.GetFuture();
    promise.SetValue(std::make_unique<TObTpccTransaction>(Session_));
    return future;
}

namespace {

class TObOwnedTpccSession : public ITpccSession {
public:
    explicit TObOwnedTpccSession(TObConnectionPool::TSessionGuard guard)
        : Guard_(std::move(guard))
        , Inner_(*Guard_)
    {}

    TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel isolation) override {
        return Inner_.Begin(isolation);
    }

private:
    TObConnectionPool::TSessionGuard Guard_;
    TObTpccSession Inner_;
};

} // namespace

TObSessionFactory::TObSessionFactory(TObConnectionPool& pool)
    : Pool_(pool)
{}

std::unique_ptr<ITpccSession> TObSessionFactory::CreateSession() {
    return std::make_unique<TObOwnedTpccSession>(Pool_.AcquireGuard());
}

std::unique_ptr<ITpccSession> TObSessionFactory::TryCreateSession() {
    auto guard = Pool_.TryAcquireGuard();
    if (!guard) {
        return nullptr;
    }
    return std::make_unique<TObOwnedTpccSession>(std::move(*guard));
}

} // namespace NTpcc

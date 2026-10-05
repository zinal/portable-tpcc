#include "dummy_session.h"

#include <constants.h>
#include <future_util.h>
#include <money.h>

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace NTpcc {

namespace {

TOperationResult FailOp(EErrorClass cls, std::string message) {
    TOperationResult r;
    r.Ok = false;
    r.ErrorClass = cls;
    r.Message = std::move(message);
    return r;
}

TOperationResult OkOp(size_t expected, size_t actual, TOperationPayload payload = {}) {
    TOperationResult r;
    r.Ok = true;
    r.ExpectedRows = expected;
    r.ActualRows = actual;
    r.Payload = std::move(payload);
    return r;
}

TCustomerRow MakeCustomer(int customerId, const std::string& lastName) {
    TCustomerRow cust;
    cust.CustomerID = customerId > 0 ? customerId : 1;
    cust.First = "Dummy";
    cust.Middle = "OE";
    cust.Last = lastName.empty() ? "BARBARBAR" : lastName;
    cust.Street1 = "Street1";
    cust.Street2 = "Street2";
    cust.City = "City";
    cust.State = "ST";
    cust.Zip = "123456789";
    cust.Phone = "1234567890123456";
    cust.Since = "2020-01-01 00:00:00";
    cust.Credit = (cust.CustomerID % 10 == 0) ? "BC" : "GC";
    cust.CreditLimit = TMoney::FromCents(5000000);
    cust.Discount = TRate::FromPermille(10);
    cust.Balance = TMoney::FromCents(-1000);
    cust.YtdPayment = TMoney::FromCents(1000);
    cust.PaymentCount = 1;
    cust.DeliveryCount = 0;
    return cust;
}

TOperationResult ExecuteSemanticOp(const TSemanticOp& op) {
    return std::visit([](const auto& typed) -> TOperationResult {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, TGetCustomerById>) {
            return OkOp(1, 1, MakeCustomer(typed.CustomerID, {}));
        } else if constexpr (std::is_same_v<T, TGetCustomersByLastName>) {
            std::vector<TCustomerRow> rows;
            rows.push_back(MakeCustomer(1, typed.LastName));
            return OkOp(rows.size(), rows.size(), std::move(rows));
        } else if constexpr (std::is_same_v<T, TGetWarehouseTax>) {
            return OkOp(1, 1, TRate::FromPermille(100));
        } else if constexpr (std::is_same_v<T, TReserveDistrictOrderId>) {
            TDistrictOrderReservation reserved;
            reserved.NextOrderID = 3001;
            reserved.DistrictTax = TRate::FromPermille(100);
            return OkOp(1, 1, reserved);
        } else if constexpr (std::is_same_v<T, TCreateOrder>) {
            return OkOp(2, 2);
        } else if constexpr (std::is_same_v<T, TGetItems>) {
            for (int itemId : typed.ItemIDs) {
                if (itemId == INVALID_ITEM_ID) {
                    return FailOp(EErrorClass::Integrity, "item not found");
                }
            }
            std::vector<TItemRow> items;
            items.reserve(typed.ItemIDs.size());
            for (int itemId : typed.ItemIDs) {
                TItemRow item;
                item.ItemID = itemId;
                item.Price = TMoney::FromCents(100);
                item.Name = "DummyItem";
                item.Data = "dummy-item-data";
                items.push_back(std::move(item));
            }
            return OkOp(items.size(), items.size(), std::move(items));
        } else if constexpr (std::is_same_v<T, TGetStocksForUpdate>) {
            std::vector<TStockRow> stocks;
            stocks.reserve(typed.Stocks.size());
            for (const auto& key : typed.Stocks) {
                TStockRow row;
                row.WarehouseID = key.WarehouseID;
                row.ItemID = key.ItemID;
                row.Quantity = 100;
                row.Ytd = TMoney::FromCents(0);
                row.OrderCount = 0;
                row.RemoteCount = 0;
                row.Data = "dummy-stock-data";
                row.DistInfo = "dummy-dist-info----------";
                stocks.push_back(std::move(row));
            }
            return OkOp(stocks.size(), stocks.size(), std::move(stocks));
        } else if constexpr (std::is_same_v<T, TUpdateStock>) {
            return OkOp(1, 1);
        } else if constexpr (std::is_same_v<T, TInsertOrderLine>) {
            return OkOp(1, 1);
        } else if constexpr (std::is_same_v<T, TApplyPaymentToLocation>) {
            TWarehouseDistrictInfo loc;
            loc.WarehouseName = "DummyWH";
            loc.WarehouseStreet1 = "WStreet1";
            loc.WarehouseStreet2 = "WStreet2";
            loc.WarehouseCity = "WCity";
            loc.WarehouseState = "ST";
            loc.WarehouseZip = "123456789";
            loc.DistrictName = "DummyDst";
            loc.DistrictStreet1 = "DStreet1";
            loc.DistrictStreet2 = "DStreet2";
            loc.DistrictCity = "DCity";
            loc.DistrictState = "ST";
            loc.DistrictZip = "123456789";
            return OkOp(2, 2, loc);
        } else if constexpr (std::is_same_v<T, TGetCustomerData>) {
            return OkOp(1, 1, std::string(50, 'x'));
        } else if constexpr (std::is_same_v<T, TUpdateCustomerPayment>) {
            return OkOp(1, 1);
        } else if constexpr (std::is_same_v<T, TInsertPaymentHistory>) {
            return OkOp(1, 1);
        } else if constexpr (std::is_same_v<T, TGetLatestCustomerOrder>) {
            TOrderHeader header;
            header.OrderID = 1;
            header.CustomerID = typed.CustomerID;
            header.CarrierID = 1;
            header.EntryDate = "2020-01-01 00:00:00";
            return OkOp(1, 1, header);
        } else if constexpr (std::is_same_v<T, TGetOrderStatusLines>) {
            std::vector<TOrderStatusLine> lines(5);
            for (size_t i = 0; i < lines.size(); ++i) {
                lines[i].ItemID = static_cast<int>(i + 1);
                lines[i].SupplyWarehouseID = typed.WarehouseID;
                lines[i].Quantity = 1;
                lines[i].Amount = TMoney::FromCents(100);
                lines[i].DeliveryDate = "2020-01-01 00:00:00";
            }
            return OkOp(lines.size(), lines.size(), std::move(lines));
        } else if constexpr (std::is_same_v<T, TGetOldestNewOrder>) {
            return OkOp(1, 1, 1);
        } else if constexpr (std::is_same_v<T, TGetDeliveryOrderInfo>) {
            TDeliveryOrderInfo info;
            info.CustomerID = 1;
            info.TotalAmount = TMoney::FromCents(500);
            info.LineCount = 5;
            return OkOp(1, 1, info);
        } else if constexpr (std::is_same_v<T, TCompleteOrderDelivery>) {
            return OkOp(3, 3);
        } else if constexpr (std::is_same_v<T, TApplyDeliveryToCustomer>) {
            return OkOp(1, 1);
        } else if constexpr (std::is_same_v<T, TCountRecentLowStock>) {
            return OkOp(1, 1, 0);
        } else {
            return FailOp(EErrorClass::Permanent, "semantic op not bound");
        }
    }, op);
}

TCommitResult MakeCommit(ECommitOutcome outcome) {
    TCommitResult r;
    r.Outcome = outcome;
    if (outcome == ECommitOutcome::Committed) {
        r.ErrorClass = EErrorClass::Permanent;
    } else if (outcome == ECommitOutcome::RolledBack) {
        r.ErrorClass = EErrorClass::NotCommitted;
    }
    return r;
}

} // anonymous

TDummyTpccTransaction::TDummyTpccTransaction(TDummyDelayConfig delay, IExecutor* executor)
    : Delay_(delay)
    , Executor_(executor)
{
}

template <typename T>
TFuture<T> TDummyTpccTransaction::Delayed(T value) const {
    return CompleteAfterDelay(Executor_, SampleDummyDelay(Delay_), std::move(value));
}

TOperationResult TDummyTpccTransaction::ExecuteImpl(const TSemanticOp& op) const {
    if (Terminal_) {
        return FailOp(EErrorClass::Permanent, "transaction already finished");
    }
    return ExecuteSemanticOp(op);
}

TCommitResult TDummyTpccTransaction::CommitImpl(ECommitOutcome outcome) const {
    TCommitResult r = MakeCommit(outcome);
    if (Terminal_ && outcome == ECommitOutcome::Committed) {
        r.Outcome = ECommitOutcome::OutcomeUnknown;
        r.ErrorClass = EErrorClass::Permanent;
        r.Message = "transaction already finished";
    }
    return r;
}

TFuture<TOperationResult> TDummyTpccTransaction::Execute(const TSemanticOp& op) {
    return Delayed(ExecuteImpl(op));
}

TFuture<TBatchResult> TDummyTpccTransaction::ExecuteBatch(const std::vector<TSemanticOp>& ops) {
    TBatchResult acc;
    acc.Ok = true;
    for (const auto& op : ops) {
        auto one = ExecuteImpl(op);
        acc.Results.push_back(std::move(one));
        if (!acc.Results.back().Ok) {
            acc.Ok = false;
            acc.ErrorClass = acc.Results.back().ErrorClass;
            acc.NativeCode = acc.Results.back().NativeCode;
            acc.Message = acc.Results.back().Message;
            break;
        }
    }
    return Delayed(std::move(acc));
}

TFuture<TFinalCommitResult> TDummyTpccTransaction::ExecuteFinalAndCommit(const TSemanticOp& op) {
    TFinalCommitResult out;
    out.Operation = ExecuteImpl(op);
    if (!out.Operation.Ok) {
        Terminal_ = true;
        out.Commit = MakeCommit(ECommitOutcome::RolledBack);
        return Delayed(std::move(out));
    }
    Terminal_ = true;
    out.Commit = MakeCommit(ECommitOutcome::Committed);
    return Delayed(std::move(out));
}

TFuture<TCommitResult> TDummyTpccTransaction::Commit() {
    auto result = CommitImpl(ECommitOutcome::Committed);
    Terminal_ = true;
    return Delayed(std::move(result));
}

TFuture<TCommitResult> TDummyTpccTransaction::Rollback() {
    auto result = CommitImpl(ECommitOutcome::RolledBack);
    Terminal_ = true;
    return Delayed(std::move(result));
}

TFuture<TCommitResult> TDummyTpccTransaction::Cancel() {
    return Then(Rollback(), [](TCommitResult result) {
        result.ErrorClass = EErrorClass::Cancelled;
        return result;
    });
}

TFuture<TOperationResult> TDummyTpccTransaction::ExecuteSelect1() {
    return Delayed(OkOp(1, 1));
}

TDummyTpccSession::TDummyTpccSession(TDummyDelayConfig delay, IExecutor* executor)
    : Delay_(delay)
    , Executor_(executor)
{
}

TFuture<std::unique_ptr<ITpccTransaction>> TDummyTpccSession::Begin(EIsolationLevel /*isolation*/) {
    // Begin does not simulate a DBMS round-trip (matches PostgreSQL).
    return MakeReadyFuture(
        std::unique_ptr<ITpccTransaction>(std::make_unique<TDummyTpccTransaction>(Delay_, Executor_)));
}

TDummySessionFactory::TDummySessionFactory(TDummyDelayConfig delay, size_t ioThreads)
    : Delay_(delay)
{
    ValidateDummyDelayConfig(Delay_);
    if (Delay_.MaxUs > 0) {
        Pool_ = std::make_unique<TThreadPool>(ioThreads == 0 ? 1 : ioThreads);
        Executor_ = Pool_.get();
    }
}

std::unique_ptr<ITpccSession> TDummySessionFactory::CreateSession() {
    return std::make_unique<TDummyTpccSession>(Delay_, Executor_);
}

} // namespace NTpcc

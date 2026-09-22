#include "tpcc_session.h"

#include "ob_batch.h"
#include "ob_connection.h"

#include <future_util.h>
#include <money.h>

#include <algorithm>
#include <optional>
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

TBatchResult OkBatch(size_t count) {
    TBatchResult batch;
    batch.Ok = true;
    batch.Results.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        batch.Results.push_back(OkOp(1, 1));
    }
    return batch;
}

TBatchResult FailBatch(EErrorClass cls, std::string message, std::string code = {}) {
    TBatchResult batch;
    batch.Ok = false;
    batch.ErrorClass = cls;
    batch.Message = std::move(message);
    batch.NativeCode = std::move(code);
    return batch;
}

TFinalCommitResult FailFinal(EErrorClass cls, std::string message, std::string code = {}) {
    TFinalCommitResult out;
    out.Operation = FailOp(cls, message, code);
    out.Commit = {
        cls == EErrorClass::AmbiguousCommit ? ECommitOutcome::OutcomeUnknown
                                            : ECommitOutcome::RolledBack,
        cls,
        code,
        out.Operation.Message};
    return out;
}

int DistrictIndex(int districtId) {
    return districtId - DISTRICT_LOW_ID;
}

bool NextRow(QueryResult& result) {
    return result.TryNextRow();
}

TFuture<TOperationResult> ReadyOp(TOperationResult result) {
    return MakeReadyFuture(std::move(result));
}

TFuture<TCommitResult> ReadyCommit(TCommitResult result) {
    return MakeReadyFuture(std::move(result));
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
    cust.Since = result.GetString(17);
    return cust;
}

TStockRow ReadStockBatch(QueryResult& result, int districtId) {
    TStockRow row;
    row.WarehouseID = result.GetInt32(0);
    row.ItemID = result.GetInt32(1);
    row.Quantity = result.GetInt32(2);
    row.Ytd = result.GetMoney(3);
    row.OrderCount = result.GetInt32(4);
    row.RemoteCount = result.GetInt32(5);
    row.Data = result.GetString(6);
    if (districtId >= 1 && districtId <= 10) {
        row.DistInfo = result.GetString(static_cast<size_t>(6 + districtId));
    }
    return row;
}

TItemRow ReadItem(QueryResult& result) {
    TItemRow item;
    item.ItemID = result.GetInt32(0);
    item.Price = result.GetMoney(1);
    item.Name = result.GetString(2);
    item.Data = result.GetString(3);
    return item;
}

TFuture<TOperationResult> MapAffected(
    TFuture<uint64_t> future,
    uint64_t expected,
    const char* what)
{
    return Then(std::move(future), [expected, what](uint64_t actual) {
        auto check = CheckAffected(actual, expected, what);
        if (!check.Ok) {
            return check;
        }
        return OkOp(expected, actual);
    });
}

} // namespace

TObTpccTransaction::TObTpccTransaction(TObSession& session)
    : Session_(session)
{}

TFuture<TOperationResult> TObTpccTransaction::CatchOp(TFuture<TOperationResult> future) {
    return CatchToValue(std::move(future), [this](const std::exception& ex) {
        return FailOp(Classifier_.ClassifyException(ex), ex.what(), ObNativeCodeOf(ex));
    });
}

TFuture<TBatchResult> TObTpccTransaction::CatchBatch(TFuture<TBatchResult> future) {
    return CatchToValue(std::move(future), [this](const std::exception& ex) {
        return FailBatch(Classifier_.ClassifyException(ex), ex.what(), ObNativeCodeOf(ex));
    });
}

TFuture<TFinalCommitResult> TObTpccTransaction::CatchFinal(
    TFuture<TFinalCommitResult> future,
    bool commitAttempted)
{
    return CatchToValue(std::move(future), [this, commitAttempted](const std::exception& ex) {
        Terminal_ = true;
        ResetTxnState();
        const auto cls = commitAttempted
            ? Classifier_.ClassifyCommitException(ex)
            : Classifier_.ClassifyException(ex);
        return FailFinal(cls, ex.what(), ObNativeCodeOf(ex));
    });
}

void TObTpccTransaction::ResetTxnState() {
    PaymentLocationApplied_ = false;
    PendingPaymentUpdate_.reset();
    DeliveryPrefetch_ = {};
}

TFuture<TOperationResult> TObTpccTransaction::ExecuteSelect1() {
    if (Terminal_) {
        return ReadyOp(FailOp(EErrorClass::Permanent, "ExecuteSelect1 called in terminal state"));
    }
    return CatchOp(Then(
        Session_.ExecuteQuery(EObQueryId::SimulationSelectCastInt, MakeParams(1)),
        [](QueryResult) { return OkOp(1, 1); }));
}

TFuture<TCommitResult> TObTpccTransaction::Commit() {
    if (Terminal_) {
        return ReadyCommit({
            ECommitOutcome::OutcomeUnknown,
            EErrorClass::Permanent,
            {},
            "Commit called in terminal state"});
    }
    return CatchToValue(
        Then(Session_.Commit(), [this]() {
            Terminal_ = true;
            ResetTxnState();
            return TCommitResult{ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}};
        }),
        [this](const std::exception& ex) {
            Terminal_ = true;
            ResetTxnState();
            const auto cls = Classifier_.ClassifyCommitException(ex);
            return TCommitResult{
                cls == EErrorClass::AmbiguousCommit ? ECommitOutcome::OutcomeUnknown
                                                    : ECommitOutcome::RolledBack,
                cls,
                ObNativeCodeOf(ex),
                ex.what()};
        });
}

TFuture<TCommitResult> TObTpccTransaction::Rollback() {
    if (Terminal_) {
        return ReadyCommit({
            ECommitOutcome::OutcomeUnknown,
            EErrorClass::Permanent,
            {},
            "Rollback called in terminal state"});
    }
    return CatchToValue(
        Then(Session_.Rollback(), [this]() {
            Terminal_ = true;
            ResetTxnState();
            return TCommitResult{ECommitOutcome::RolledBack, EErrorClass::Permanent, {}, {}};
        }),
        [this](const std::exception& ex) {
            Terminal_ = true;
            ResetTxnState();
            return TCommitResult{
                ECommitOutcome::OutcomeUnknown,
                Classifier_.ClassifyException(ex),
                ObNativeCodeOf(ex),
                ex.what()};
        });
}

TFuture<TCommitResult> TObTpccTransaction::Cancel() {
    return Then(Rollback(), [](TCommitResult result) {
        result.ErrorClass = EErrorClass::Cancelled;
        return result;
    });
}

TFuture<TBatchResult> TObTpccTransaction::ExecuteBatch(const std::vector<TSemanticOp>& ops) {
    if (Terminal_) {
        return MakeReadyFuture(FailBatch(EErrorClass::Permanent, "ExecuteBatch called in terminal state"));
    }
    if (ops.empty()) {
        TBatchResult empty;
        empty.Ok = true;
        return MakeReadyFuture(std::move(empty));
    }
    if (AllSemanticOpsAre<TUpdateStock>(ops)) {
        return ExecuteStockBatch(ops);
    }
    if (AllSemanticOpsAre<TInsertOrderLine>(ops)) {
        return ExecuteOrderLineBatch(ops);
    }
    if (AllSemanticOpsAre<TCompleteOrderDelivery>(ops)) {
        return ExecuteCompleteDeliveryBatch(ops);
    }
    if (AllSemanticOpsAre<TApplyDeliveryToCustomer>(ops)) {
        return ExecuteApplyDeliveryBatch(ops);
    }
    return ExecuteBatchSequentially(std::vector<TSemanticOp>(ops));
}

TFuture<TBatchResult> TObTpccTransaction::ExecuteBatchSequentially(std::vector<TSemanticOp> ops) {
    TBatchResult init;
    init.Ok = true;
    return ThenFold(
        std::move(ops),
        std::move(init),
        [this](TBatchResult acc, const TSemanticOp& item) -> TFuture<TBatchResult> {
            if (!acc.Ok) {
                return MakeReadyFuture(std::move(acc));
            }
            return Then(Execute(item), [acc = std::move(acc)](TOperationResult one) mutable {
                acc.Results.push_back(std::move(one));
                if (!acc.Results.back().Ok) {
                    acc.Ok = false;
                    acc.ErrorClass = acc.Results.back().ErrorClass;
                    acc.NativeCode = acc.Results.back().NativeCode;
                    acc.Message = acc.Results.back().Message;
                }
                return acc;
            });
        });
}

TFuture<TBatchResult> TObTpccTransaction::ExecuteStockBatch(const std::vector<TSemanticOp>& ops) {
    const auto rows = AggregateObStockUpdates(ops);
    if (rows.empty() || rows.size() > ObBatchMaxRows) {
        return ExecuteBatchSequentially(std::vector<TSemanticOp>(ops));
    }
    TObParams params;
    params.Reserve(6 * rows.size());
    for (const auto& row : rows) {
        params(row.WarehouseID)(row.ItemID)(row.NewQuantity)
            (row.OrderedQuantity)(row.LineCount)(row.RemoteIncrement);
    }
    const size_t opCount = ops.size();
    const size_t expected = rows.size();
    return CatchBatch(Then(
        Session_.ExecuteModify(BuildObStockUpdateBatchSql(rows.size()), params),
        [opCount, expected](uint64_t affected) {
            auto check = CheckAffected(affected, expected, "stock update batch");
            if (!check.Ok) {
                return FailBatch(check.ErrorClass, check.Message, check.NativeCode);
            }
            return OkBatch(opCount);
        }));
}

TFuture<TBatchResult> TObTpccTransaction::ExecuteOrderLineBatch(const std::vector<TSemanticOp>& ops) {
    if (ops.size() > ObBatchMaxRows) {
        return ExecuteBatchSequentially(std::vector<TSemanticOp>(ops));
    }
    TObParams params;
    params.Reserve(9 * ops.size());
    for (const auto& op : ops) {
        const auto& line = std::get<TInsertOrderLine>(op);
        params(line.OrderID)(line.DistrictID)(line.WarehouseID)(line.LineNumber)
            (line.ItemID)(line.SupplyWarehouseID)(line.Quantity)
            (line.Amount.ToString())(line.DistInfo);
    }
    const size_t opCount = ops.size();
    return CatchBatch(Then(
        Session_.ExecuteModify(BuildObOrderLineInsertSql(ops.size()), params),
        [opCount](uint64_t affected) {
            auto check = CheckAffected(affected, opCount, "order_line insert batch");
            if (!check.Ok) {
                return FailBatch(check.ErrorClass, check.Message, check.NativeCode);
            }
            return OkBatch(opCount);
        }));
}

TFuture<TBatchResult> TObTpccTransaction::ExecuteCompleteDeliveryBatch(
    const std::vector<TSemanticOp>& ops)
{
    std::vector<TObDeliveryOrderKey> keys;
    keys.reserve(ops.size());
    int warehouseId = 0;
    int carrierId = 0;
    uint64_t expectedLines = 0;
    for (const auto& op : ops) {
        const auto& row = std::get<TCompleteOrderDelivery>(op);
        if (keys.empty()) {
            warehouseId = row.WarehouseID;
            carrierId = row.CarrierID;
        }
        keys.push_back(TObDeliveryOrderKey{row.DistrictID, row.OrderID});
        expectedLines += static_cast<uint64_t>(row.LineCount);
    }
    std::sort(keys.begin(), keys.end(), [](const TObDeliveryOrderKey& a, const TObDeliveryOrderKey& b) {
        if (a.DistrictID != b.DistrictID) {
            return a.DistrictID < b.DistrictID;
        }
        return a.OrderID < b.OrderID;
    });
    const size_t n = ops.size();
    return CatchBatch(Then(
        Session_.ExecuteMulti(BuildObDeliveryCompleteSql(warehouseId, carrierId, keys)),
        [n, expectedLines](TObMultiResult result) {
            if (result.Affected.size() < 3) {
                return FailBatch(EErrorClass::Integrity, "delivery complete: missing affected counts");
            }
            if (result.Affected[0] == 0 || result.Affected[0] < n) {
                return FailBatch(
                    EErrorClass::RetryableAbort,
                    "new_order row already claimed by concurrent delivery");
            }
            auto deleted = CheckAffected(result.Affected[0], n, "new_order delivery delete");
            if (!deleted.Ok) {
                return FailBatch(deleted.ErrorClass, deleted.Message, deleted.NativeCode);
            }
            auto orders = CheckAffected(result.Affected[1], n, "order carrier update");
            if (!orders.Ok) {
                return FailBatch(orders.ErrorClass, orders.Message, orders.NativeCode);
            }
            auto lines = CheckAffected(result.Affected[2], expectedLines, "order_line delivery update");
            if (!lines.Ok) {
                return FailBatch(lines.ErrorClass, lines.Message, lines.NativeCode);
            }
            return OkBatch(n);
        }));
}

TFuture<TBatchResult> TObTpccTransaction::ExecuteApplyDeliveryBatch(
    const std::vector<TSemanticOp>& ops)
{
    if (ops.size() > static_cast<size_t>(DISTRICT_COUNT)) {
        return ExecuteBatchSequentially(std::vector<TSemanticOp>(ops));
    }
    TObParams params;
    params.Reserve(4 * ops.size());
    for (const auto& op : ops) {
        const auto& row = std::get<TApplyDeliveryToCustomer>(op);
        params(row.WarehouseID)(row.DistrictID)(row.CustomerID)(row.Amount.ToString());
    }
    const size_t n = ops.size();
    return CatchBatch(Then(
        Session_.ExecuteModify(BuildObDeliveryApplySql(ops.size()), params),
        [n](uint64_t affected) {
            auto check = CheckAffected(affected, n, "customer delivery update");
            if (!check.Ok) {
                return FailBatch(check.ErrorClass, check.Message, check.NativeCode);
            }
            return OkBatch(n);
        }));
}

TFuture<TFinalCommitResult> TObTpccTransaction::ExecuteFinalAndCommit(const TSemanticOp& op) {
    if (Terminal_) {
        return MakeReadyFuture(FailFinal(EErrorClass::Permanent, "ExecuteFinalAndCommit called in terminal state"));
    }
    if (PendingPaymentUpdate_) {
        if (const auto* history = std::get_if<TInsertPaymentHistory>(&op)) {
            auto update = *PendingPaymentUpdate_;
            PendingPaymentUpdate_.reset();
            return FinishPayment(update, *history);
        }
    }
    if (const auto* apply = std::get_if<TApplyDeliveryToCustomer>(&op)) {
        return FinishApplyDelivery(*apply);
    }
    return Then(Execute(op), [this](TOperationResult operation) {
        return CommitAfterOperation(std::move(operation));
    });
}

TFuture<TFinalCommitResult> TObTpccTransaction::CommitAfterOperation(TOperationResult operation) {
    TFinalCommitResult out;
    out.Operation = std::move(operation);
    if (!out.Operation.Ok) {
        return Then(Rollback(), [out = std::move(out)](TCommitResult commit) mutable {
            out.Commit = std::move(commit);
            return out;
        });
    }
    return Then(Commit(), [out = std::move(out)](TCommitResult commit) mutable {
        out.Commit = std::move(commit);
        return out;
    });
}

TFuture<TFinalCommitResult> TObTpccTransaction::FinishPayment(
    const TUpdateCustomerPayment& update,
    const TInsertPaymentHistory& history)
{
    std::string quotedData = "''";
    std::string quotedHistory = "''";
    try {
        if (update.UpdateData) {
            quotedData = QuoteSqlString(update.NewData);
        }
        quotedHistory = QuoteSqlString(history.Data);
    } catch (const std::exception& ex) {
        return CommitAfterOperation(FailOp(EErrorClass::Permanent, ex.what()));
    }
    // DML and COMMIT are separate: mysql_real_query of a multi-statement
    // runs COMMIT before the client can inspect earlier affected-row counts.
    return CatchFinal(
        Then(
            Session_.ExecuteMulti(
                BuildObPaymentFinishSql(update, history, quotedData, quotedHistory)),
            [this](TObMultiResult result) -> TFuture<TFinalCommitResult> {
                if (result.Affected.size() < 2) {
                    return CommitAfterOperation(FailOp(
                        EErrorClass::Integrity, "payment finish: missing affected counts"));
                }
                auto customer = CheckAffected(result.Affected[0], 1, "customer payment update");
                if (!customer.Ok) {
                    return CommitAfterOperation(std::move(customer));
                }
                auto hist = CheckAffected(result.Affected[1], 1, "history insert");
                if (!hist.Ok) {
                    return CommitAfterOperation(std::move(hist));
                }
                return CommitAfterOperation(OkOp(1, 1));
            }),
        false);
}

TFuture<TFinalCommitResult> TObTpccTransaction::FinishApplyDelivery(const TApplyDeliveryToCustomer& apply) {
    return CatchFinal(
        Then(
            Session_.ExecuteMulti(BuildObDeliveryFinishSql(apply)),
            [this](TObMultiResult result) -> TFuture<TFinalCommitResult> {
                if (result.Affected.empty()) {
                    return CommitAfterOperation(FailOp(
                        EErrorClass::Integrity, "customer delivery update: missing affected count"));
                }
                return CommitAfterOperation(
                    CheckAffected(result.Affected[0], 1, "customer delivery update"));
            }),
        false);
}

TOperationResult TObTpccTransaction::OldestFromCache(int districtId) const {
    const int idx = DistrictIndex(districtId);
    if (idx < 0 || idx >= DISTRICT_COUNT) {
        return FailOp(EErrorClass::Permanent, "delivery district out of range");
    }
    if (!DeliveryPrefetch_.OldestOrderId[idx]) {
        return OkOp(0, 0, 0);
    }
    return OkOp(1, 1, *DeliveryPrefetch_.OldestOrderId[idx]);
}

TOperationResult TObTpccTransaction::DeliveryInfoFromCache(int districtId, int orderId) const {
    const int idx = DistrictIndex(districtId);
    if (idx < 0 || idx >= DISTRICT_COUNT || !DeliveryPrefetch_.Info[idx]) {
        return FailOp(EErrorClass::Integrity, "order not found");
    }
    if (DeliveryPrefetch_.OldestOrderId[idx] && *DeliveryPrefetch_.OldestOrderId[idx] != orderId) {
        return FailOp(EErrorClass::Integrity, "order not found");
    }
    return OkOp(1, 1, *DeliveryPrefetch_.Info[idx]);
}

TFuture<TOperationResult> TObTpccTransaction::EnsureDeliveryPrefetch(int warehouseId) {
    if (DeliveryPrefetch_.Loaded && DeliveryPrefetch_.WarehouseID == warehouseId) {
        return ReadyOp(OkOp(1, 1));
    }
    DeliveryPrefetch_ = {};
    DeliveryPrefetch_.WarehouseID = warehouseId;
    return Then(
        Session_.ExecuteMulti(BuildObOldestNewOrdersSql(warehouseId)),
        [this, warehouseId](TObMultiResult oldest) -> TFuture<TOperationResult> {
            if (oldest.Selects.size() != static_cast<size_t>(DISTRICT_COUNT)) {
                return ReadyOp(FailOp(EErrorClass::Integrity, "delivery oldest prefetch size mismatch"));
            }
            std::vector<TObDeliveryOrderKey> found;
            found.reserve(DISTRICT_COUNT);
            for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
                const int idx = DistrictIndex(d);
                auto& result = oldest.Selects[static_cast<size_t>(idx)];
                result.Reset();
                if (result.TryNextRow()) {
                    const int orderId = result.GetInt32(0);
                    DeliveryPrefetch_.OldestOrderId[idx] = orderId;
                    found.push_back(TObDeliveryOrderKey{d, orderId});
                }
            }
            if (found.empty()) {
                DeliveryPrefetch_.Loaded = true;
                return ReadyOp(OkOp(0, 0));
            }
            return Then(
                Session_.ExecuteMulti(BuildObDeliveryOrderInfoSql(warehouseId, found)),
                [this, found](TObMultiResult info) {
                    if (info.Selects.size() < 2) {
                        return FailOp(EErrorClass::Integrity, "delivery info prefetch size mismatch");
                    }
                    auto orders = info.Selects[0];
                    while (orders.TryNextRow()) {
                        const int districtId = orders.GetInt32(0);
                        const int idx = DistrictIndex(districtId);
                        if (idx < 0 || idx >= DISTRICT_COUNT) {
                            continue;
                        }
                        TDeliveryOrderInfo row;
                        row.CustomerID = orders.GetInt32(2);
                        DeliveryPrefetch_.Info[idx] = row;
                    }
                    auto lines = info.Selects[1];
                    while (lines.TryNextRow()) {
                        const int districtId = lines.GetInt32(0);
                        const int idx = DistrictIndex(districtId);
                        if (idx < 0 || idx >= DISTRICT_COUNT || !DeliveryPrefetch_.Info[idx]) {
                            continue;
                        }
                        auto& row = *DeliveryPrefetch_.Info[idx];
                        row.TotalAmount += lines.GetMoney(2);
                        row.LineCount += 1;
                    }
                    for (const auto& key : found) {
                        const int idx = DistrictIndex(key.DistrictID);
                        if (idx < 0 || idx >= DISTRICT_COUNT || !DeliveryPrefetch_.Info[idx]) {
                            return FailOp(EErrorClass::Integrity, "order not found");
                        }
                    }
                    DeliveryPrefetch_.Loaded = true;
                    return OkOp(found.size(), found.size());
                });
        });
}

TFuture<TOperationResult> TObTpccTransaction::Execute(const TSemanticOp& op) {
    if (Terminal_) {
        return ReadyOp(FailOp(EErrorClass::Permanent, "Execute called in terminal state"));
    }
    if (const auto* p = std::get_if<TGetWarehouseTax>(&op)) {
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::GetWarehouseTax,
                MakeParams(p->WarehouseID)),
            [](QueryResult result) {
                if (!NextRow(result)) {
                    return FailOp(EErrorClass::Integrity, "warehouse not found");
                }
                return OkOp(1, 1, result.GetRate(0));
            }));
    }
    if (const auto* p = std::get_if<TReserveDistrictOrderId>(&op)) {
        const int warehouseId = p->WarehouseID;
        const int districtId = p->DistrictID;
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::ReserveDistrictOrderId,
                MakeParams(warehouseId, districtId)),
            [this, warehouseId, districtId](QueryResult result) -> TFuture<TOperationResult> {
                if (!NextRow(result)) {
                    return ReadyOp(FailOp(EErrorClass::Integrity, "district not found"));
                }
                const int nextId = result.GetInt32(0);
                const auto tax = result.GetRate(1);
                return Then(
                    Session_.ExecuteModify(
                        EObQueryId::UpdateDistrictNextOrderId,
                        MakeParams(nextId + 1, warehouseId, districtId)),
                    [nextId, tax](uint64_t affected) {
                        auto check = CheckAffected(affected, 1, "district next order update");
                        if (!check.Ok) {
                            return check;
                        }
                        TDistrictOrderReservation res;
                        res.NextOrderID = nextId;
                        res.DistrictTax = tax;
                        return OkOp(1, 1, res);
                    });
            }));
    }
    if (const auto* p = std::get_if<TGetCustomerById>(&op)) {
        const auto queryId = PaymentLocationApplied_
            ? EObQueryId::GetCustomerByIdForUpdate
            : EObQueryId::GetCustomerById;
        return CatchOp(Then(
            Session_.ExecuteQuery(
                queryId,
                MakeParams(p->WarehouseID, p->DistrictID, p->CustomerID)),
            [](QueryResult result) {
                if (!NextRow(result)) {
                    return FailOp(EErrorClass::Integrity, "customer not found");
                }
                return OkOp(1, 1, ReadCustomer(result));
            }));
    }
    if (const auto* p = std::get_if<TGetItems>(&op)) {
        const auto ids = UniqueItemIds(p->ItemIDs);
        if (ids.empty()) {
            return ReadyOp(OkOp(0, 0, std::vector<TItemRow>{}));
        }
        if (ids.size() > ObBatchMaxRows) {
            return ReadyOp(FailOp(EErrorClass::Permanent, "too many items in TGetItems"));
        }
        TObParams params;
        params.Reserve(ids.size());
        for (int id : ids) {
            params(id);
        }
        const size_t expected = ids.size();
        return CatchOp(Then(
            Session_.ExecuteQuery(BuildObGetItemsSql(ids.size()), params),
            [expected](QueryResult result) {
                std::vector<TItemRow> items;
                items.reserve(expected);
                while (NextRow(result)) {
                    items.push_back(ReadItem(result));
                }
                if (items.size() != expected) {
                    return FailOp(EErrorClass::Integrity, "item not found");
                }
                return OkOp(expected, items.size(), std::move(items));
            }));
    }
    if (const auto* p = std::get_if<TCreateOrder>(&op)) {
        const auto req = *p;
        return CatchOp(Then(
            Session_.ExecuteModify(
                EObQueryId::CreateOrder,
                MakeParams(req.OrderID, req.DistrictID, req.WarehouseID, req.CustomerID,
                           req.LineCount, req.AllLocal)),
            [this, req](uint64_t orderAffected) -> TFuture<TOperationResult> {
                auto orderCheck = CheckAffected(orderAffected, 1, "oorder insert");
                if (!orderCheck.Ok) {
                    return ReadyOp(std::move(orderCheck));
                }
                return Then(
                    Session_.ExecuteModify(
                        EObQueryId::CreateNewOrder,
                        MakeParams(req.OrderID, req.DistrictID, req.WarehouseID)),
                    [](uint64_t newOrderAffected) {
                        auto newOrderCheck = CheckAffected(newOrderAffected, 1, "new_order insert");
                        if (!newOrderCheck.Ok) {
                            return newOrderCheck;
                        }
                        return OkOp(2, 2);
                    });
            }));
    }
    if (const auto* p = std::get_if<TUpdateStock>(&op)) {
        return CatchOp(MapAffected(
            Session_.ExecuteModify(
                EObQueryId::UpdateStock,
                MakeParams(p->NewQuantity, p->OrderedQuantity, p->RemoteIncrement,
                           p->WarehouseID, p->ItemID)),
            1,
            "stock update"));
    }
    if (const auto* p = std::get_if<TInsertOrderLine>(&op)) {
        return CatchOp(MapAffected(
            Session_.ExecuteModify(
                EObQueryId::InsertOrderLine,
                MakeParams(p->OrderID, p->DistrictID, p->WarehouseID, p->LineNumber,
                           p->ItemID, p->SupplyWarehouseID, p->Quantity,
                           p->Amount.ToString(), p->DistInfo)),
            1,
            "order_line insert"));
    }
    if (const auto* p = std::get_if<TCountRecentLowStock>(&op)) {
        const auto req = *p;
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::CountRecentDistrict,
                MakeParams(req.WarehouseID, req.DistrictID)),
            [this, req](QueryResult dist) -> TFuture<TOperationResult> {
                if (!NextRow(dist)) {
                    return ReadyOp(FailOp(EErrorClass::Integrity, "district not found"));
                }
                const int nextOid = dist.GetInt32(0);
                return Then(
                    Session_.ExecuteQuery(
                        EObQueryId::CountRecentLowStock,
                        MakeParams(req.WarehouseID, req.DistrictID, nextOid,
                                   nextOid - req.RecentOrderCount, req.WarehouseID, req.Threshold)),
                    [](QueryResult stock) {
                        int count = 0;
                        if (NextRow(stock)) {
                            count = stock.GetInt32(0);
                        }
                        return OkOp(1, 1, count);
                    });
            }));
    }
    if (const auto* p = std::get_if<TGetOldestNewOrder>(&op)) {
        const int warehouseId = p->WarehouseID;
        const int districtId = p->DistrictID;
        return CatchOp(Then(
            EnsureDeliveryPrefetch(warehouseId),
            [this, districtId](TOperationResult prefetch) {
                if (!prefetch.Ok) {
                    return prefetch;
                }
                return OldestFromCache(districtId);
            }));
    }
    if (const auto* p = std::get_if<TApplyPaymentToLocation>(&op)) {
        const auto req = *p;
        return CatchOp(Then(
            Session_.ExecuteMulti(
                BuildObPaymentLocationSql(req.WarehouseID, req.DistrictID, req.Amount.ToString())),
            [this](TObMultiResult result) {
                if (result.Affected.empty() || result.Affected[0] == 0) {
                    return FailOp(EErrorClass::Integrity, "warehouse or district payment update");
                }
                if (result.Selects.empty() || !NextRow(result.Selects[0])) {
                    return FailOp(EErrorClass::Integrity, "warehouse or district not found");
                }
                auto& row = result.Selects[0];
                TWarehouseDistrictInfo info;
                info.WarehouseName = row.GetString(0);
                info.WarehouseStreet1 = row.GetString(1);
                info.WarehouseStreet2 = row.GetString(2);
                info.WarehouseCity = row.GetString(3);
                info.WarehouseState = row.GetString(4);
                info.WarehouseZip = row.GetString(5);
                info.DistrictName = row.GetString(6);
                info.DistrictStreet1 = row.GetString(7);
                info.DistrictStreet2 = row.GetString(8);
                info.DistrictCity = row.GetString(9);
                info.DistrictState = row.GetString(10);
                info.DistrictZip = row.GetString(11);
                PaymentLocationApplied_ = true;
                return OkOp(2, 2, std::move(info));
            }));
    }
    if (const auto* p = std::get_if<TInsertPaymentHistory>(&op)) {
        return CatchOp(MapAffected(
            Session_.ExecuteModify(
                EObQueryId::InsertPaymentHistory,
                MakeParams(p->CustomerID, p->CustomerDistrictID, p->CustomerWarehouseID,
                           p->PaymentDistrictID, p->PaymentWarehouseID,
                           p->Amount.ToString(), p->Data)),
            1,
            "history insert"));
    }
    if (const auto* p = std::get_if<TGetCustomersByLastName>(&op)) {
        const auto queryId = PaymentLocationApplied_
            ? EObQueryId::GetCustomersByLastNameForUpdate
            : EObQueryId::GetCustomersByLastName;
        return CatchOp(Then(
            Session_.ExecuteQuery(
                queryId,
                MakeParams(p->WarehouseID, p->DistrictID, p->LastName)),
            [](QueryResult result) {
                std::vector<TCustomerRow> customers;
                while (NextRow(result)) {
                    customers.push_back(ReadCustomer(result));
                }
                return OkOp(customers.size(), customers.size(), std::move(customers));
            }));
    }
    if (const auto* p = std::get_if<TGetStocksForUpdate>(&op)) {
        const auto keys = UniqueSortedStockKeys(p->Stocks);
        const int districtId = p->DistrictID;
        if (keys.empty()) {
            return ReadyOp(OkOp(0, 0, std::vector<TStockRow>{}));
        }
        if (keys.size() > ObBatchMaxRows) {
            return ReadyOp(FailOp(EErrorClass::Permanent, "too many stock keys in TGetStocksForUpdate"));
        }
        TObParams params;
        params.Reserve(2 * keys.size());
        for (const auto& key : keys) {
            params(key.WarehouseID)(key.ItemID);
        }
        const size_t expected = keys.size();
        return CatchOp(Then(
            Session_.ExecuteQuery(BuildObGetStocksForUpdateSql(keys.size()), params),
            [expected, districtId](QueryResult result) {
                std::vector<TStockRow> rows;
                rows.reserve(expected);
                while (NextRow(result)) {
                    rows.push_back(ReadStockBatch(result, districtId));
                }
                if (rows.size() != expected) {
                    return FailOp(EErrorClass::Integrity, "stock not found");
                }
                return OkOp(expected, rows.size(), std::move(rows));
            }));
    }
    if (const auto* p = std::get_if<TGetCustomerData>(&op)) {
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::GetCustomerData,
                MakeParams(p->WarehouseID, p->DistrictID, p->CustomerID)),
            [](QueryResult result) {
                if (!NextRow(result)) {
                    return FailOp(EErrorClass::Integrity, "customer not found");
                }
                return OkOp(1, 1, result.GetString(0));
            }));
    }
    if (const auto* p = std::get_if<TUpdateCustomerPayment>(&op)) {
        PendingPaymentUpdate_ = *p;
        return ReadyOp(OkOp(1, 1));
    }
    if (const auto* p = std::get_if<TGetLatestCustomerOrder>(&op)) {
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::GetLatestCustomerOrder,
                MakeParams(p->WarehouseID, p->DistrictID, p->CustomerID)),
            [](QueryResult result) {
                if (!NextRow(result)) {
                    return OkOp(0, 0);
                }
                TOrderHeader header;
                header.OrderID = result.GetInt32(0);
                header.CustomerID = result.GetInt32(1);
                header.CarrierID = result.GetOptionalInt32(2);
                header.EntryDate = result.GetString(3);
                return OkOp(1, 1, std::move(header));
            }));
    }
    if (const auto* p = std::get_if<TGetOrderStatusLines>(&op)) {
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::GetOrderStatusLines,
                MakeParams(p->WarehouseID, p->DistrictID, p->OrderID)),
            [](QueryResult result) {
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
                return OkOp(lines.size(), lines.size(), std::move(lines));
            }));
    }
    if (const auto* p = std::get_if<TGetDeliveryOrderInfo>(&op)) {
        if (DeliveryPrefetch_.Loaded && DeliveryPrefetch_.WarehouseID == p->WarehouseID) {
            return ReadyOp(DeliveryInfoFromCache(p->DistrictID, p->OrderID));
        }
        const auto req = *p;
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::GetDeliveryOrderCustomer,
                MakeParams(req.WarehouseID, req.DistrictID, req.OrderID)),
            [this, req](QueryResult cid) -> TFuture<TOperationResult> {
                if (!NextRow(cid)) {
                    return ReadyOp(FailOp(EErrorClass::Integrity, "order not found"));
                }
                TDeliveryOrderInfo info;
                info.CustomerID = cid.GetInt32(0);
                return Then(
                    Session_.ExecuteQuery(
                        EObQueryId::GetDeliveryOrderLines,
                        MakeParams(req.WarehouseID, req.DistrictID, req.OrderID)),
                    [info = std::move(info)](QueryResult ol) mutable {
                        int64_t totalCents = 0;
                        while (NextRow(ol)) {
                            totalCents += ol.GetMoney(0).Cents();
                            ++info.LineCount;
                        }
                        info.TotalAmount = TMoney::FromCents(totalCents);
                        return OkOp(1, 1, std::move(info));
                    });
            }));
    }
    if (const auto* p = std::get_if<TCompleteOrderDelivery>(&op)) {
        const auto req = *p;
        return CatchOp(Then(
            Session_.ExecuteModify(
                EObQueryId::DeleteNewOrder,
                MakeParams(req.WarehouseID, req.DistrictID, req.OrderID)),
            [this, req](uint64_t deleted) -> TFuture<TOperationResult> {
                if (deleted == 0) {
                    return ReadyOp(FailOp(
                        EErrorClass::RetryableAbort,
                        "new_order row already claimed by concurrent delivery"));
                }
                if (deleted != 1) {
                    return ReadyOp(CheckAffected(deleted, 1, "new_order delivery delete"));
                }
                return Then(
                    Session_.ExecuteModify(
                        EObQueryId::UpdateOrderCarrier,
                        MakeParams(req.CarrierID, req.WarehouseID, req.DistrictID, req.OrderID)),
                    [this, req](uint64_t orderAffected) -> TFuture<TOperationResult> {
                        auto check = CheckAffected(orderAffected, 1, "order carrier update");
                        if (!check.Ok) {
                            return ReadyOp(std::move(check));
                        }
                        return Then(
                            Session_.ExecuteModify(
                                EObQueryId::UpdateOrderLineDelivery,
                                MakeParams(req.WarehouseID, req.DistrictID, req.OrderID)),
                            [lineCount = req.LineCount](uint64_t linesAffected) {
                                auto check = CheckAffected(
                                    linesAffected, lineCount, "order_line delivery update");
                                if (!check.Ok) {
                                    return check;
                                }
                                return OkOp(3, 3);
                            });
                    });
            }));
    }
    if (const auto* p = std::get_if<TApplyDeliveryToCustomer>(&op)) {
        return CatchOp(MapAffected(
            Session_.ExecuteModify(
                EObQueryId::ApplyDeliveryToCustomer,
                MakeParams(p->Amount.ToString(), p->WarehouseID, p->DistrictID,
                           p->CustomerID)),
            1,
            "customer delivery update"));
    }

    return ReadyOp(FailOp(EErrorClass::Permanent, "semantic op not bound in TObTpccTransaction"));
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

TFuture<std::unique_ptr<ITpccSession>> TObSessionFactory::WaitCreateSession() {
    // Get a future for the TObSession; resolves immediately when the pool has a
    // free connection, or once another terminal releases one (no polling).
    auto sessionFuture = std::make_shared<TFuture<TObSession>>(Pool_.AcquireSessionAsync());
    auto sharedPromise = std::make_shared<TPromise<std::unique_ptr<ITpccSession>>>();
    auto result = sharedPromise->GetFuture();

    sessionFuture->Subscribe([sessionFuture, sharedPromise, pool = &Pool_]() mutable {
        try {
            auto session = sessionFuture->Get();
            auto guard = TObConnectionPool::TSessionGuard(*pool, std::move(session));
            sharedPromise->SetValue(
                std::make_unique<TObOwnedTpccSession>(std::move(guard)));
        } catch (...) {
            sharedPromise->SetException(std::current_exception());
        }
    });

    return result;
}

} // namespace NTpcc
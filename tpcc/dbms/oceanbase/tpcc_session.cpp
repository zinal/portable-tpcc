#include "tpcc_session.h"

#include <future_util.h>
#include <money.h>

#include <optional>
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
    cust.Data = result.GetString(17);
    cust.Since = result.GetString(18);
    return cust;
}

TStockRow ReadStock(QueryResult& result, const TStockKey& key, int districtId) {
    TStockRow row;
    row.WarehouseID = key.WarehouseID;
    row.ItemID = key.ItemID;
    row.Quantity = result.GetInt32(0);
    row.Ytd = result.GetMoney(1);
    row.OrderCount = result.GetInt32(2);
    row.RemoteCount = result.GetInt32(3);
    row.Data = result.GetString(4);
    if (districtId >= 1 && districtId <= 10) {
        row.DistInfo = result.GetString(static_cast<size_t>(4 + districtId));
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
            return TCommitResult{ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}};
        }),
        [this](const std::exception& ex) {
            Terminal_ = true;
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
            return TCommitResult{ECommitOutcome::RolledBack, EErrorClass::Permanent, {}, {}};
        }),
        [this](const std::exception& ex) {
            Terminal_ = true;
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
    TBatchResult init;
    init.Ok = true;
    return ThenFold(
        std::vector<TSemanticOp>(ops),
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

TFuture<TFinalCommitResult> TObTpccTransaction::ExecuteFinalAndCommit(const TSemanticOp& op) {
    return Then(Execute(op), [this](TOperationResult operation) -> TFuture<TFinalCommitResult> {
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
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::GetCustomerById,
                MakeParams(p->WarehouseID, p->DistrictID, p->CustomerID)),
            [](QueryResult result) {
                if (!NextRow(result)) {
                    return FailOp(EErrorClass::Integrity, "customer not found");
                }
                return OkOp(1, 1, ReadCustomer(result));
            }));
    }
    if (const auto* p = std::get_if<TGetItems>(&op)) {
        struct TAcc {
            std::vector<TItemRow> Items;
            std::optional<TOperationResult> Failure;
        };
        const size_t n = p->ItemIDs.size();
        TAcc init;
        init.Items.reserve(n);
        return CatchOp(Then(
            ThenFold(
                p->ItemIDs,
                std::move(init),
                [this](TAcc acc, int id) -> TFuture<TAcc> {
                    if (acc.Failure) {
                        return MakeReadyFuture(std::move(acc));
                    }
                    return Then(
                        Session_.ExecuteQuery(EObQueryId::GetItems, MakeParams(id)),
                        [acc = std::move(acc)](QueryResult result) mutable {
                            if (!NextRow(result)) {
                                acc.Failure = FailOp(EErrorClass::Integrity, "item not found");
                                return acc;
                            }
                            acc.Items.push_back(ReadItem(result));
                            return acc;
                        });
                }),
            [n](TAcc acc) {
                if (acc.Failure) {
                    return *acc.Failure;
                }
                return OkOp(n, acc.Items.size(), std::move(acc.Items));
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
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::GetOldestNewOrder,
                MakeParams(p->WarehouseID, p->DistrictID)),
            [](QueryResult result) {
                if (!NextRow(result)) {
                    return OkOp(0, 0, 0);
                }
                return OkOp(1, 1, result.GetInt32(0));
            }));
    }
    if (const auto* p = std::get_if<TApplyPaymentToLocation>(&op)) {
        const auto req = *p;
        const auto amount = req.Amount.ToString();
        return CatchOp(Then(
            Session_.ExecuteModify(
                EObQueryId::ApplyPaymentWarehouse,
                MakeParams(amount, req.WarehouseID)),
            [this, req, amount](uint64_t whAffected) -> TFuture<TOperationResult> {
                auto check = CheckAffected(whAffected, 1, "warehouse payment update");
                if (!check.Ok) {
                    return ReadyOp(std::move(check));
                }
                return Then(
                    Session_.ExecuteModify(
                        EObQueryId::ApplyPaymentDistrict,
                        MakeParams(amount, req.WarehouseID, req.DistrictID)),
                    [this, req](uint64_t distAffected) -> TFuture<TOperationResult> {
                        auto check = CheckAffected(distAffected, 1, "district payment update");
                        if (!check.Ok) {
                            return ReadyOp(std::move(check));
                        }
                        return Then(
                            Session_.ExecuteQuery(
                                EObQueryId::SelectPaymentWarehouse,
                                MakeParams(req.WarehouseID)),
                            [this, req](QueryResult wh) -> TFuture<TOperationResult> {
                                TWarehouseDistrictInfo info;
                                if (NextRow(wh)) {
                                    info.WarehouseName = wh.GetString(0);
                                    info.WarehouseStreet1 = wh.GetString(1);
                                    info.WarehouseStreet2 = wh.GetString(2);
                                    info.WarehouseCity = wh.GetString(3);
                                    info.WarehouseState = wh.GetString(4);
                                    info.WarehouseZip = wh.GetString(5);
                                }
                                return Then(
                                    Session_.ExecuteQuery(
                                        EObQueryId::SelectPaymentDistrict,
                                        MakeParams(req.WarehouseID, req.DistrictID)),
                                    [info = std::move(info)](QueryResult dist) mutable {
                                        if (NextRow(dist)) {
                                            info.DistrictName = dist.GetString(0);
                                            info.DistrictStreet1 = dist.GetString(1);
                                            info.DistrictStreet2 = dist.GetString(2);
                                            info.DistrictCity = dist.GetString(3);
                                            info.DistrictState = dist.GetString(4);
                                            info.DistrictZip = dist.GetString(5);
                                        }
                                        return OkOp(2, 2, std::move(info));
                                    });
                            });
                    });
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
        return CatchOp(Then(
            Session_.ExecuteQuery(
                EObQueryId::GetCustomersByLastName,
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
        struct TAcc {
            std::vector<TStockRow> Rows;
            std::optional<TOperationResult> Failure;
        };
        const int districtId = p->DistrictID;
        const size_t n = p->Stocks.size();
        TAcc init;
        init.Rows.reserve(n);
        return CatchOp(Then(
            ThenFold(
                p->Stocks,
                std::move(init),
                [this, districtId](TAcc acc, TStockKey key) -> TFuture<TAcc> {
                    if (acc.Failure) {
                        return MakeReadyFuture(std::move(acc));
                    }
                    return Then(
                        Session_.ExecuteQuery(
                            EObQueryId::GetStockForUpdate,
                            MakeParams(key.WarehouseID, key.ItemID)),
                        [acc = std::move(acc), key, districtId](QueryResult result) mutable {
                            if (!NextRow(result)) {
                                acc.Failure = FailOp(EErrorClass::Integrity, "stock not found");
                                return acc;
                            }
                            acc.Rows.push_back(ReadStock(result, key, districtId));
                            return acc;
                        });
                }),
            [n](TAcc acc) {
                if (acc.Failure) {
                    return *acc.Failure;
                }
                return OkOp(n, acc.Rows.size(), std::move(acc.Rows));
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
        if (p->UpdateData) {
            return CatchOp(MapAffected(
                Session_.ExecuteModify(
                    EObQueryId::UpdateCustomerPaymentWithData,
                    MakeParams(p->NewBalance.ToString(), p->NewYtdPayment.ToString(),
                               p->NewPaymentCount, p->NewData,
                               p->WarehouseID, p->DistrictID, p->CustomerID)),
                1,
                "customer payment update"));
        }
        return CatchOp(MapAffected(
            Session_.ExecuteModify(
                EObQueryId::UpdateCustomerPayment,
                MakeParams(p->NewBalance.ToString(), p->NewYtdPayment.ToString(),
                           p->NewPaymentCount,
                           p->WarehouseID, p->DistrictID, p->CustomerID)),
            1,
            "customer payment update"));
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

} // namespace NTpcc

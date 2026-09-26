#include "ydb_session.h"

#include "ydb_batch.h"
#include "ydb_future.h"
#include "ydb_tx_mode.h"
#include "ydb_value_parse.h"

#include <future_util.h>
#include <log.h>
#include <money.h>

#include <fmt/format.h>
#include <util/datetime/base.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/params/params.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <constants.h>

#include <atomic>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

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
    return MakeReadyFuture(std::move(result));
}

TFuture<TCommitResult> ReadyCommit(TCommitResult result) {
    return MakeReadyFuture(std::move(result));
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
    out.Operation = FailOp(cls, std::move(message), code);
    out.Commit = {
        cls == EErrorClass::AmbiguousCommit ? ECommitOutcome::OutcomeUnknown
                                            : ECommitOutcome::RolledBack,
        cls,
        std::move(code),
        out.Operation.Message};
    return out;
}

// ExecuteQuery concatenates every FORMAT_VALUE part into one ResultSet.
// RowsCount() is that fragment's rows_size — the same bound TryNextRow uses.
size_t CountRows(const NYdb::TResultSet& resultSet) {
    return resultSet.RowsCount();
}

std::optional<std::pair<EErrorClass, const char*>> DeliveryCardinalityError(
    uint64_t newOrders,
    uint64_t orders,
    uint64_t lines,
    uint64_t n,
    uint64_t expectedLines)
{
    if (newOrders < n) {
        return std::make_pair(
            EErrorClass::RetryableAbort,
            "new_order row already claimed by concurrent delivery");
    }
    if (newOrders != n) {
        return std::make_pair(EErrorClass::Integrity, "new_order delivery delete");
    }
    if (orders != n) {
        return std::make_pair(EErrorClass::Integrity, "order carrier update");
    }
    if (lines != expectedLines) {
        return std::make_pair(EErrorClass::Integrity, "order_line delivery update");
    }
    return std::nullopt;
}

std::string OldestNewOrdersSql() {
    std::string sql = "DECLARE $w_id AS Int32;\n";
    for (int districtId = DISTRICT_LOW_ID; districtId <= DISTRICT_HIGH_ID; ++districtId) {
        sql += fmt::format(
            "SELECT no_o_id FROM `new_order` "
            "WHERE no_w_id = $w_id AND no_d_id = {} "
            "ORDER BY no_o_id ASC LIMIT 1;\n",
            districtId);
    }
    return sql;
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
    return TMoney::Parse(DecimalFromValue(parser.ColumnParser(column)).ToString());
}

TRate ParseRate(NYdb::TResultSetParser& parser, const char* column) {
    return TRate::Parse(DecimalFromValue(parser.ColumnParser(column)).ToString());
}

std::string ParseUtf8(NYdb::TResultSetParser& parser, const char* column) {
    return Utf8FromValue(parser.ColumnParser(column));
}

int32_t ParseInt32(NYdb::TResultSetParser& parser, const char* column) {
    return Int32FromValue(parser.ColumnParser(column));
}

uint64_t ParseCount(NYdb::TResultSetParser& parser, const char* column) {
    return CountFromValue(parser.ColumnParser(column));
}

uint64_t CountColumn(const NYdb::TResultSet& resultSet) {
    NYdb::TResultSetParser parser(resultSet);
    if (!parser.TryNextRow()) {
        return 0;
    }
    return ParseCount(parser, "n");
}

std::optional<int32_t> ParseOptionalInt32(NYdb::TResultSetParser& parser, const char* column) {
    return OptionalInt32FromValue(parser.ColumnParser(column));
}

std::optional<TInstant> ParseOptionalTimestamp(NYdb::TResultSetParser& parser, const char* column) {
    return OptionalTimestampFromValue(parser.ColumnParser(column));
}

TCustomerRow ParseCustomer(NYdb::TResultSetParser& parser) {
    TCustomerRow cust;
    cust.CustomerID = ParseInt32(parser, "c_id");
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
    cust.PaymentCount = ParseInt32(parser, "c_payment_cnt");
    cust.DeliveryCount = ParseInt32(parser, "c_delivery_cnt");
    if (auto ts = ParseOptionalTimestamp(parser, "c_since")) {
        cust.Since = TimestampToString(*ts);
    }
    return cust;
}

// Technical history key (TPC-C 1.3.1 has no PK). 22-bit process salt in bits
// 41..62, bit 63 clear, 41-bit counter. Load hist_id stays below bit 41, and a
// zero salt is rejected so payment inserts do not reuse that range. Two
// workers therefore cannot overwrite each other's history row.
int64_t NextHistoryId() {
    static const uint64_t salt = [] {
        std::random_device device;
        uint64_t value = 0;
        do {
            value = static_cast<uint64_t>(device()) & 0x3FFFFFull;
        } while (value == 0);
        return value << 41;
    }();
    static std::atomic<uint64_t> seq{0};
    constexpr uint64_t counterMask = (uint64_t{1} << 41) - 1;
    const uint64_t counter = seq.fetch_add(1, std::memory_order_relaxed) & counterMask;
    return static_cast<int64_t>(salt | counter);
}

} // anonymous

TYdbTpccTransaction::TYdbTpccTransaction(
    TSession session,
    std::string path,
    TTxSettings txSettings)
    : Session_(std::move(session))
    , TxSettings_(std::move(txSettings))
    , Path_(std::move(path))
{}

TFuture<TOperationResult> TYdbTpccTransaction::CatchOp(TFuture<TOperationResult> future) {
    return CatchToValue(std::move(future), [this](const std::exception& ex) {
        if (const auto* ydb = dynamic_cast<const NYdb::NStatusHelpers::TYdbErrorException*>(&ex)) {
            const auto& status = ydb->GetStatus();
            return FailOp(
                Classifier_.ClassifyStatus(status),
                YdbIssuesToString(status),
                YdbStatusCodeOf(status.GetStatus()));
        }
        return FailOp(Classifier_.ClassifyException(ex), ex.what());
    });
}

TFuture<TBatchResult> TYdbTpccTransaction::CatchBatch(TFuture<TBatchResult> future) {
    return CatchToValue(std::move(future), [this](const std::exception& ex) {
        if (const auto* ydb = dynamic_cast<const NYdb::NStatusHelpers::TYdbErrorException*>(&ex)) {
            const auto& status = ydb->GetStatus();
            return FailBatch(
                Classifier_.ClassifyStatus(status),
                YdbIssuesToString(status),
                YdbStatusCodeOf(status.GetStatus()));
        }
        return FailBatch(Classifier_.ClassifyException(ex), ex.what());
    });
}

TFuture<TExecuteQueryResult> TYdbTpccTransaction::ExecQuery(
    std::string query,
    std::optional<TParams> params,
    bool commit)
{
    TTxControl txControl = Tx_
        ? TTxControl::Tx(*Tx_)
        : TTxControl::BeginTx(TxSettings_);
    if (commit) {
        txControl.CommitTx(true);
    }
    auto ydbFuture = !params
        ? Session_.ExecuteQuery(query, txControl)
        : Session_.ExecuteQuery(query, txControl, *params);
    return Then(BridgeYdbFuture(std::move(ydbFuture)), [this, commit](TExecuteQueryResult result) {
        if (!result.IsSuccess()) {
            throw NYdb::NStatusHelpers::TYdbErrorException(std::move(result));
        }
        if (commit) {
            Terminal_ = true;
            Tx_.reset();
            ResetTxnState();
        } else if (auto tx = result.GetTransaction()) {
            Tx_ = std::move(*tx);
        }
        return result;
    });
}

TFuture<TCommitResult> TYdbTpccTransaction::Commit() {
    if (Terminal_) {
        return ReadyCommit({ECommitOutcome::OutcomeUnknown, EErrorClass::Permanent, {}, "Commit called in terminal state"});
    }
    if (!Tx_) {
        Terminal_ = true;
        ResetTxnState();
        return ReadyCommit({ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}});
    }
    return CatchToValue(
        Then(BridgeYdbFuture(Tx_->Commit()), [this](NYdb::NQuery::TCommitTransactionResult status) {
            Terminal_ = true;
            Tx_.reset();
            ResetTxnState();
            if (status.IsSuccess()) {
                return TCommitResult{ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}};
            }
            const auto cls = Classifier_.ClassifyStatus(status, true);
            return TCommitResult{
                cls == EErrorClass::AmbiguousCommit ? ECommitOutcome::OutcomeUnknown : ECommitOutcome::RolledBack,
                cls,
                YdbStatusCodeOf(status.GetStatus()),
                YdbIssuesToString(status)};
        }),
        [this](const std::exception& ex) {
            Terminal_ = true;
            Tx_.reset();
            ResetTxnState();
            const auto cls = Classifier_.ClassifyException(ex, true);
            return TCommitResult{
                cls == EErrorClass::AmbiguousCommit ? ECommitOutcome::OutcomeUnknown : ECommitOutcome::RolledBack,
                cls,
                {},
                ex.what()};
        });
}

TFuture<TCommitResult> TYdbTpccTransaction::Rollback() {
    if (Terminal_) {
        return ReadyCommit({ECommitOutcome::OutcomeUnknown, EErrorClass::Permanent, {}, "Rollback called in terminal state"});
    }
    if (!Tx_) {
        Terminal_ = true;
        ResetTxnState();
        return ReadyCommit({ECommitOutcome::RolledBack, EErrorClass::Permanent, {}, {}});
    }
    return CatchToValue(
        Then(BridgeYdbFuture(Tx_->Rollback()), [this](NYdb::TStatus status) {
            Terminal_ = true;
            Tx_.reset();
            ResetTxnState();
            if (status.IsSuccess()) {
                return TCommitResult{ECommitOutcome::RolledBack, EErrorClass::Permanent, {}, {}};
            }
            return TCommitResult{
                ECommitOutcome::OutcomeUnknown,
                Classifier_.ClassifyStatus(status),
                YdbStatusCodeOf(status.GetStatus()),
                YdbIssuesToString(status)};
        }),
        [this](const std::exception& ex) {
            Terminal_ = true;
            Tx_.reset();
            ResetTxnState();
            return TCommitResult{ECommitOutcome::OutcomeUnknown, Classifier_.ClassifyException(ex), {}, ex.what()};
        });
}

void TYdbTpccTransaction::ResetTxnState() {
    PendingPaymentUpdate_.reset();
    DeliveryPrefetch_ = {};
}

TFuture<TOperationResult> TYdbTpccTransaction::RollbackThenFailOp(
    EErrorClass cls,
    std::string message)
{
    return Then(Rollback(), [cls, message = std::move(message)](TCommitResult) {
        return FailOp(cls, message);
    });
}

TFuture<TBatchResult> TYdbTpccTransaction::RollbackThenFailBatch(
    EErrorClass cls,
    std::string message)
{
    return Then(Rollback(), [cls, message = std::move(message)](TCommitResult) {
        return FailBatch(cls, message);
    });
}

TFuture<TCommitResult> TYdbTpccTransaction::Cancel() {
    return Then(Rollback(), [](TCommitResult result) {
        result.ErrorClass = EErrorClass::Cancelled;
        return result;
    });
}

TFuture<TBatchResult> TYdbTpccTransaction::ExecuteBatch(const std::vector<TSemanticOp>& ops) {
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

TFuture<TBatchResult> TYdbTpccTransaction::ExecuteBatchSequentially(std::vector<TSemanticOp> ops) {
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

TFuture<TBatchResult> TYdbTpccTransaction::ExecuteStockBatch(const std::vector<TSemanticOp>& ops) {
    const auto rows = AggregateYdbStockUpdates(ops);
    TParamsBuilder builder;
    auto& params = builder.AddParam("$values").BeginList();
    for (const auto& row : rows) {
        params.AddListItem()
            .BeginStruct()
            .AddMember("w_id").Int32(row.WarehouseID)
            .AddMember("i_id").Int32(row.ItemID)
            .AddMember("quantity").Int32(row.NewQuantity)
            .AddMember("ytd_inc").Decimal(TDecimalValue(
                std::to_string(row.OrderedQuantity), MONEY_PRECISION, MONEY_SCALE))
            .AddMember("line_cnt").Int32(row.LineCount)
            .AddMember("remote_inc").Int32(row.RemoteIncrement)
            .EndStruct();
    }
    auto built = params.EndList().Build().Build();
    const size_t opCount = ops.size();
    const size_t expected = rows.size();
    // Incremental UPDATE, same as the single-row statement. A missing stock
    // row is not inserted. The following SELECT is the postcondition: Query
    // Service does not report affected rows.
    return CatchBatch(Then(
        ExecQuery(Prefix(Path_) + R"(
                DECLARE $values AS List<Struct<
                    w_id:Int32, i_id:Int32, quantity:Int32,
                    ytd_inc:Decimal(22,9), line_cnt:Int32, remote_inc:Int32>>;
                $keys = ListMap($values, ($row) -> (AsTuple($row.w_id, $row.i_id)));
                UPDATE `stock` ON
                SELECT
                    u.w_id AS s_w_id,
                    u.i_id AS s_i_id,
                    u.quantity AS s_quantity,
                    s.s_ytd + u.ytd_inc AS s_ytd,
                    s.s_order_cnt + u.line_cnt AS s_order_cnt,
                    s.s_remote_cnt + u.remote_inc AS s_remote_cnt
                FROM AS_TABLE($values) AS u
                INNER JOIN `stock` AS s
                    ON s.s_w_id = u.w_id AND s.s_i_id = u.i_id;
                SELECT COUNT(*) AS n FROM `stock` WHERE (s_w_id, s_i_id) IN $keys;
            )", std::move(built)),
        [this, opCount, expected](TExecuteQueryResult result) -> TFuture<TBatchResult> {
            if (CountColumn(result.GetResultSet(0)) != expected) {
                return RollbackThenFailBatch(EErrorClass::Integrity, "stock update batch");
            }
            return MakeReadyFuture(OkBatch(opCount));
        }));
}

TFuture<TBatchResult> TYdbTpccTransaction::ExecuteOrderLineBatch(const std::vector<TSemanticOp>& ops) {
    TParamsBuilder builder;
    auto& params = builder.AddParam("$values").BeginList();
    for (const auto& op : ops) {
        const auto& line = std::get<TInsertOrderLine>(op);
        params.AddListItem()
            .BeginStruct()
            .AddMember("w_id").Int32(line.WarehouseID)
            .AddMember("d_id").Int32(line.DistrictID)
            .AddMember("o_id").Int32(line.OrderID)
            .AddMember("number").Int32(line.LineNumber)
            .AddMember("i_id").Int32(line.ItemID)
            .AddMember("amount").Decimal(Decimal(line.Amount))
            .AddMember("supply_w_id").Int32(line.SupplyWarehouseID)
            .AddMember("quantity").Int32(line.Quantity)
            .AddMember("dist_info").Utf8(line.DistInfo)
            .EndStruct();
    }
    auto built = params.EndList().Build().Build();
    const size_t opCount = ops.size();
    // Named projection, not SELECT *: a column list plus SELECT * fails YQL
    // type annotation. Aliases match that list; a name mismatch is warning
    // 4517 even though values are bound by position. ol_delivery_d is omitted
    // so it stays NULL.
    return CatchBatch(Then(
        ExecQuery(Prefix(Path_) + R"(
                DECLARE $values AS List<Struct<
                    w_id:Int32, d_id:Int32, o_id:Int32, number:Int32, i_id:Int32,
                    amount:Decimal(22,9), supply_w_id:Int32, quantity:Int32, dist_info:Utf8>>;
                INSERT INTO `order_line` (
                    ol_w_id, ol_d_id, ol_o_id, ol_number, ol_i_id,
                    ol_amount, ol_supply_w_id, ol_quantity, ol_dist_info)
                SELECT
                    w_id AS ol_w_id,
                    d_id AS ol_d_id,
                    o_id AS ol_o_id,
                    number AS ol_number,
                    i_id AS ol_i_id,
                    amount AS ol_amount,
                    supply_w_id AS ol_supply_w_id,
                    quantity AS ol_quantity,
                    dist_info AS ol_dist_info
                FROM AS_TABLE($values);
            )", std::move(built)),
        [opCount](TExecuteQueryResult) {
            return OkBatch(opCount);
        }));
}

TFuture<TBatchResult> TYdbTpccTransaction::ExecuteCompleteDeliveryBatch(const std::vector<TSemanticOp>& ops) {
    const int carrierId = std::get<TCompleteOrderDelivery>(ops.front()).CarrierID;
    TParamsBuilder builder;
    auto& values = builder.AddParam("$values").BeginList();
    for (const auto& op : ops) {
        const auto& row = std::get<TCompleteOrderDelivery>(op);
        values.AddListItem()
            .BeginStruct()
            .AddMember("w_id").Int32(row.WarehouseID)
            .AddMember("d_id").Int32(row.DistrictID)
            .AddMember("o_id").Int32(row.OrderID)
            .EndStruct();
    }
    auto built = values.EndList().Build()
        .AddParam("$carrier_id").Int32(carrierId).Build()
        .Build();
    const size_t opCount = ops.size();
    uint64_t expectedLines = 0;
    for (const auto& op : ops) {
        expectedLines += static_cast<uint64_t>(std::get<TCompleteOrderDelivery>(op).LineCount);
    }
    // SELECTs run before the DML so the counts are the pre-image. A short
    // new_order count is a concurrent claim; carrier is UPDATE, not UPSERT.
    return CatchBatch(Then(
        ExecQuery(Prefix(Path_) + R"(
                DECLARE $values AS List<Struct<w_id:Int32, d_id:Int32, o_id:Int32>>;
                DECLARE $carrier_id AS Int32;
                $keys = ListMap($values, ($row) -> (AsTuple($row.w_id, $row.d_id, $row.o_id)));
                SELECT COUNT(*) AS n FROM `new_order`
                 WHERE (no_w_id, no_d_id, no_o_id) IN $keys;
                SELECT COUNT(*) AS n FROM `oorder`
                 WHERE (o_w_id, o_d_id, o_id) IN $keys;
                SELECT COUNT(*) AS n FROM `order_line`
                 WHERE (ol_w_id, ol_d_id, ol_o_id) IN $keys;
                DELETE FROM `new_order`
                 WHERE (no_w_id, no_d_id, no_o_id) IN $keys;
                UPDATE `oorder`
                   SET o_carrier_id = $carrier_id
                 WHERE (o_w_id, o_d_id, o_id) IN $keys;
                UPDATE `order_line`
                   SET ol_delivery_d = CurrentUtcTimestamp()
                 WHERE (ol_w_id, ol_d_id, ol_o_id) IN $keys;
            )", std::move(built)),
        [this, opCount, expectedLines](TExecuteQueryResult result) -> TFuture<TBatchResult> {
            if (auto error = DeliveryCardinalityError(
                    CountColumn(result.GetResultSet(0)),
                    CountColumn(result.GetResultSet(1)),
                    CountColumn(result.GetResultSet(2)),
                    opCount,
                    expectedLines))
            {
                return RollbackThenFailBatch(error->first, error->second);
            }
            return MakeReadyFuture(OkBatch(opCount));
        }));
}

TFuture<TBatchResult> TYdbTpccTransaction::ExecuteApplyDeliveryBatch(const std::vector<TSemanticOp>& ops) {
    TParamsBuilder builder;
    auto& params = builder.AddParam("$values").BeginList();
    for (const auto& op : ops) {
        const auto& row = std::get<TApplyDeliveryToCustomer>(op);
        params.AddListItem()
            .BeginStruct()
            .AddMember("w_id").Int32(row.WarehouseID)
            .AddMember("d_id").Int32(row.DistrictID)
            .AddMember("c_id").Int32(row.CustomerID)
            .AddMember("amount").Decimal(Decimal(row.Amount))
            .EndStruct();
    }
    auto built = params.EndList().Build().Build();
    const size_t opCount = ops.size();
    return CatchBatch(Then(
        ExecQuery(Prefix(Path_) + R"(
                DECLARE $values AS List<Struct<
                    w_id:Int32, d_id:Int32, c_id:Int32, amount:Decimal(22,9)>>;
                $keys = ListMap($values, ($row) -> (AsTuple($row.w_id, $row.d_id, $row.c_id)));
                UPDATE `customer` ON
                SELECT
                    u.w_id AS c_w_id,
                    u.d_id AS c_d_id,
                    u.c_id AS c_id,
                    c.c_balance + u.amount AS c_balance,
                    c.c_delivery_cnt + 1 AS c_delivery_cnt
                FROM AS_TABLE($values) AS u
                INNER JOIN `customer` AS c
                    ON c.c_w_id = u.w_id AND c.c_d_id = u.d_id AND c.c_id = u.c_id;
                SELECT COUNT(*) AS n FROM `customer`
                 WHERE (c_w_id, c_d_id, c_id) IN $keys;
            )", std::move(built)),
        [this, opCount](TExecuteQueryResult result) -> TFuture<TBatchResult> {
            if (CountColumn(result.GetResultSet(0)) != opCount) {
                return RollbackThenFailBatch(EErrorClass::Integrity, "customer delivery update");
            }
            return MakeReadyFuture(OkBatch(opCount));
        }));
}

TFuture<TFinalCommitResult> TYdbTpccTransaction::CommitAfterOperation(TOperationResult operation) {
    TFinalCommitResult out;
    out.Operation = std::move(operation);
    if (!out.Operation.Ok) {
        return Then(Rollback(), [out = std::move(out)](TCommitResult commit) mutable {
            out.Commit = std::move(commit);
            return out;
        });
    }
    if (Terminal_) {
        out.Commit = {ECommitOutcome::Committed, EErrorClass::Permanent, {}, {}};
        return MakeReadyFuture(std::move(out));
    }
    return Then(Commit(), [out = std::move(out)](TCommitResult commit) mutable {
        out.Commit = std::move(commit);
        return out;
    });
}

TFuture<TFinalCommitResult> TYdbTpccTransaction::FinishPayment(
    const TUpdateCustomerPayment& update,
    const TInsertPaymentHistory& history)
{
    TParamsBuilder builder;
    builder
        .AddParam("$w_id").Int32(update.WarehouseID).Build()
        .AddParam("$d_id").Int32(update.DistrictID).Build()
        .AddParam("$c_id").Int32(update.CustomerID).Build()
        .AddParam("$balance").Decimal(Decimal(update.NewBalance)).Build()
        .AddParam("$ytd_payment").Decimal(Decimal(update.NewYtdPayment)).Build()
        .AddParam("$payment_cnt").Int32(update.NewPaymentCount).Build()
        .AddParam("$h_w_id").Int32(history.PaymentWarehouseID).Build()
        .AddParam("$hist_id").Int64(NextHistoryId()).Build()
        .AddParam("$h_c_w_id").Int32(history.CustomerWarehouseID).Build()
        .AddParam("$h_c_d_id").Int32(history.CustomerDistrictID).Build()
        .AddParam("$h_c_id").Int32(history.CustomerID).Build()
        .AddParam("$h_d_id").Int32(history.PaymentDistrictID).Build()
        .AddParam("$h_amount").Decimal(Decimal(history.Amount)).Build()
        .AddParam("$h_data").Utf8(history.Data).Build();
    std::string dataDeclare;
    std::string dataAssign;
    if (update.UpdateData) {
        builder.AddParam("$data").Utf8(update.NewData).Build();
        dataDeclare = "DECLARE $data AS Utf8;\n";
        dataAssign = ", c_data = $data";
    }
    auto params = builder.Build();
    // No CommitTx: the customer SELECT is checked, then Commit or Rollback.
    const std::string query = Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                DECLARE $balance AS Decimal(22,9);
                DECLARE $ytd_payment AS Decimal(22,9);
                DECLARE $payment_cnt AS Int32;
                DECLARE $h_w_id AS Int32;
                DECLARE $hist_id AS Int64;
                DECLARE $h_c_w_id AS Int32;
                DECLARE $h_c_d_id AS Int32;
                DECLARE $h_c_id AS Int32;
                DECLARE $h_d_id AS Int32;
                DECLARE $h_amount AS Decimal(22,9);
                DECLARE $h_data AS Utf8;
            )" + dataDeclare + fmt::format(R"(
                UPDATE `customer`
                   SET c_balance = $balance, c_ytd_payment = $ytd_payment,
                       c_payment_cnt = $payment_cnt{}
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
                INSERT INTO `history` (h_w_id, hist_id, h_c_w_id, h_c_d_id, h_c_id, h_d_id, h_date, h_amount, h_data)
                VALUES ($h_w_id, $hist_id, $h_c_w_id, $h_c_d_id, $h_c_id, $h_d_id,
                        CurrentUtcTimestamp(), $h_amount, $h_data);
                SELECT c_id FROM `customer`
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
            )", dataAssign);
    return Then(
        CatchOp(Then(
            ExecQuery(query, std::move(params)),
            [](TExecuteQueryResult result) -> TFuture<TOperationResult> {
                NYdb::TResultSetParser parser(result.GetResultSet(0));
                if (!parser.TryNextRow()) {
                    return ReadyOp(FailOp(EErrorClass::Integrity, "customer payment update"));
                }
                return ReadyOp(OkOp(1, 1));
            })),
        [this](TOperationResult operation) {
            return CommitAfterOperation(std::move(operation));
        });
}

TFuture<TFinalCommitResult> TYdbTpccTransaction::FinishApplyDelivery(
    const TApplyDeliveryToCustomer& apply)
{
    auto params = TParamsBuilder()
        .AddParam("$w_id").Int32(apply.WarehouseID).Build()
        .AddParam("$d_id").Int32(apply.DistrictID).Build()
        .AddParam("$c_id").Int32(apply.CustomerID).Build()
        .AddParam("$amount").Decimal(Decimal(apply.Amount)).Build()
        .Build();
    return Then(
        CatchOp(Then(
            ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                DECLARE $amount AS Decimal(22,9);
                UPDATE `customer`
                   SET c_balance = c_balance + $amount,
                       c_delivery_cnt = c_delivery_cnt + 1
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
                SELECT c_id FROM `customer`
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
            )", std::move(params)),
            [](TExecuteQueryResult result) -> TFuture<TOperationResult> {
                NYdb::TResultSetParser parser(result.GetResultSet(0));
                if (!parser.TryNextRow()) {
                    return ReadyOp(FailOp(EErrorClass::Integrity, "customer delivery update"));
                }
                return ReadyOp(OkOp(1, 1));
            })),
        [this](TOperationResult operation) {
            return CommitAfterOperation(std::move(operation));
        });
}

TFuture<TFinalCommitResult> TYdbTpccTransaction::ExecuteFinalAndCommit(const TSemanticOp& op) {
    if (Terminal_) {
        return MakeReadyFuture(FailFinal(
            EErrorClass::Permanent, "ExecuteFinalAndCommit called in terminal state"));
    }
    if (PendingPaymentUpdate_) {
        if (const auto* history = std::get_if<TInsertPaymentHistory>(&op)) {
            const auto update = *PendingPaymentUpdate_;
            PendingPaymentUpdate_.reset();
            return FinishPayment(update, *history);
        }
    }
    if (const auto* apply = std::get_if<TApplyDeliveryToCustomer>(&op)) {
        return FinishApplyDelivery(*apply);
    }
    FinalCommitMode_ = true;
    return Then(Execute(op), [this](TOperationResult operation) -> TFuture<TFinalCommitResult> {
        FinalCommitMode_ = false;
        return CommitAfterOperation(std::move(operation));
    });
}

TFuture<TOperationResult> TYdbTpccTransaction::ExecuteSelect1() {
    if (Terminal_) {
        return ReadyOp(FailOp(EErrorClass::Permanent, "ExecuteSelect1 called in terminal state"));
    }
    return CatchOp(Then(
        ExecQuery("SELECT 1 AS one;"),
        [](TExecuteQueryResult) { return OkOp(1, 1); }));
}

TFuture<TOperationResult> TYdbTpccTransaction::Execute(const TSemanticOp& op) {
    if (Terminal_) {
        return ReadyOp(FailOp(EErrorClass::Permanent, "Execute called in terminal state"));
    }

    if (const auto* p = std::get_if<TGetWarehouseTax>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                SELECT w_tax FROM `warehouse` WHERE w_id = $w_id;
            )", std::move(params)),
                [](TExecuteQueryResult result) {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    if (!parser.TryNextRow()) {
                        return FailOp(EErrorClass::Integrity, "warehouse not found");
                    }
                    return OkOp(1, 1, ParseRate(parser, "w_tax"));
                }));
        }

        if (const auto* p = std::get_if<TReserveDistrictOrderId>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .Build();
            // One round trip: read next order id, then increment. The second
            // SELECT must observe that increment before CreateOrder runs.
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                SELECT d_next_o_id, d_tax
                  FROM `district`
                 WHERE d_w_id = $w_id AND d_id = $d_id;
                UPDATE `district`
                   SET d_next_o_id = d_next_o_id + 1
                 WHERE d_w_id = $w_id AND d_id = $d_id;
                SELECT d_next_o_id
                  FROM `district`
                 WHERE d_w_id = $w_id AND d_id = $d_id;
            )", std::move(params)),
                [this](TExecuteQueryResult result) -> TFuture<TOperationResult> {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    if (!parser.TryNextRow()) {
                        return RollbackThenFailOp(EErrorClass::Integrity, "district not found");
                    }
                    TDistrictOrderReservation res;
                    res.NextOrderID = ParseInt32(parser, "d_next_o_id");
                    res.DistrictTax = ParseRate(parser, "d_tax");
                    NYdb::TResultSetParser after(result.GetResultSet(1));
                    if (!after.TryNextRow() || ParseInt32(after, "d_next_o_id") != res.NextOrderID + 1) {
                        return RollbackThenFailOp(EErrorClass::Integrity, "district next order update");
                    }
                    return ReadyOp(OkOp(1, 1, std::move(res)));
                }));
        }

        if (const auto* p = std::get_if<TGetCustomerById>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                SELECT c_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state,
                       c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment,
                       c_payment_cnt, c_delivery_cnt, c_since
                  FROM `customer`
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
            )", std::move(params)),
                [](TExecuteQueryResult result) {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    if (!parser.TryNextRow()) {
                        return FailOp(EErrorClass::Integrity, "customer not found");
                    }
                    return OkOp(1, 1, ParseCustomer(parser));
                }));
        }

        if (const auto* p = std::get_if<TGetCustomersByLastName>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_last").Utf8(p->LastName).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_last AS Utf8;
                SELECT c_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state,
                       c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment,
                       c_payment_cnt, c_delivery_cnt, c_since
                  FROM `customer` VIEW `idx_customer_name` AS idx
                 WHERE idx.c_w_id = $w_id AND idx.c_d_id = $d_id AND idx.c_last = $c_last
                 ORDER BY idx.c_first;
            )", std::move(params)),
                [](TExecuteQueryResult result) {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    std::vector<TCustomerRow> customers;
                    while (parser.TryNextRow()) {
                        customers.push_back(ParseCustomer(parser));
                    }
                    return OkOp(customers.size(), customers.size(), std::move(customers));
                }));
        }

        if (const auto* p = std::get_if<TGetItems>(&op)) {
            // ydb workload tpcc GetItems uniques IDs first. IN returns one row
            // per id; NURand item picks commonly repeat on a New-Order.
            const std::set<int> uniqueIds(p->ItemIDs.begin(), p->ItemIDs.end());
            const size_t expected = uniqueIds.size();
            TParamsBuilder builder;
            auto& params = builder.AddParam("$item_ids").BeginList();
            for (int id : uniqueIds) {
                params.AddListItem().Int32(id);
            }
            auto builtParams = params.EndList().Build().Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $item_ids AS List<Int32>;
                SELECT i_id, i_price, i_name, i_data
                  FROM `item`
                 WHERE i_id IN $item_ids
                 ORDER BY i_id;
            )", std::move(builtParams)),
                [expected](TExecuteQueryResult result) {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    std::vector<TItemRow> items;
                    while (parser.TryNextRow()) {
                        TItemRow item;
                        item.ItemID = ParseInt32(parser, "i_id");
                        item.Price = ParseMoney(parser, "i_price");
                        item.Name = ParseUtf8(parser, "i_name");
                        item.Data = ParseUtf8(parser, "i_data");
                        items.push_back(std::move(item));
                    }
                    if (items.size() != expected) {
                        return FailOp(EErrorClass::Integrity, "item not found");
                    }
                    return OkOp(expected, items.size(), std::move(items));
                }));
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
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                DECLARE $c_id AS Int32;
                DECLARE $ol_cnt AS Int32;
                DECLARE $all_local AS Int32;
                INSERT INTO `oorder` (o_w_id, o_d_id, o_id, o_c_id, o_carrier_id, o_ol_cnt, o_all_local, o_entry_d)
                VALUES ($w_id, $d_id, $o_id, $c_id, NULL, $ol_cnt, $all_local, CurrentUtcTimestamp());
                INSERT INTO `new_order` (no_w_id, no_d_id, no_o_id)
                VALUES ($w_id, $d_id, $o_id);
            )", std::move(params)),
                [](TExecuteQueryResult) { return OkOp(2, 2); }));
        }

        if (const auto* p = std::get_if<TGetStocksForUpdate>(&op)) {
            const size_t expected = p->Stocks.size();
            TParamsBuilder builder;
            auto& params = builder
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$stocks").BeginList();
            for (const auto& key : p->Stocks) {
                params.AddListItem()
                    .BeginTuple()
                    .AddElement().Int32(key.WarehouseID)
                    .AddElement().Int32(key.ItemID)
                    .EndTuple();
            }
            auto builtParams = params.EndList().Build().Build();
            // Same composite-key IN as ydb workload tpcc GetStock. A JOIN on
            // AS_TABLE($stocks) makes YQL emit qualified names (s.s_w_id), so
            // ColumnParser("s_w_id") fails with "Unknown column".
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $d_id AS Int32;
                DECLARE $stocks AS List<Tuple<Int32, Int32>>;
                SELECT s_w_id, s_i_id, s_quantity, s_ytd, s_order_cnt, s_remote_cnt, s_data,
                       CASE $d_id
                         WHEN 1 THEN s_dist_01 WHEN 2 THEN s_dist_02 WHEN 3 THEN s_dist_03
                         WHEN 4 THEN s_dist_04 WHEN 5 THEN s_dist_05 WHEN 6 THEN s_dist_06
                         WHEN 7 THEN s_dist_07 WHEN 8 THEN s_dist_08 WHEN 9 THEN s_dist_09
                         ELSE s_dist_10
                       END AS s_dist_info
                  FROM `stock`
                 WHERE (s_w_id, s_i_id) IN $stocks;
            )", std::move(builtParams)),
                [expected](TExecuteQueryResult result) {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    std::vector<TStockRow> stocks;
                    while (parser.TryNextRow()) {
                        TStockRow row;
                        row.WarehouseID = ParseInt32(parser, "s_w_id");
                        row.ItemID = ParseInt32(parser, "s_i_id");
                        row.Quantity = ParseInt32(parser, "s_quantity");
                        row.Ytd = ParseMoney(parser, "s_ytd");
                        row.OrderCount = ParseInt32(parser, "s_order_cnt");
                        row.RemoteCount = ParseInt32(parser, "s_remote_cnt");
                        row.Data = ParseUtf8(parser, "s_data");
                        row.DistInfo = ParseUtf8(parser, "s_dist_info");
                        stocks.push_back(std::move(row));
                    }
                    if (stocks.size() != expected) {
                        return FailOp(EErrorClass::Integrity, "stock not found");
                    }
                    return OkOp(expected, stocks.size(), std::move(stocks));
                }));
        }

        if (const auto* p = std::get_if<TUpdateStock>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$i_id").Int32(p->ItemID).Build()
                .AddParam("$quantity").Int32(p->NewQuantity).Build()
                .AddParam("$ordered").Decimal(TDecimalValue(std::to_string(p->OrderedQuantity), MONEY_PRECISION, MONEY_SCALE)).Build()
                .AddParam("$remote").Int32(p->RemoteIncrement).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
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
                SELECT s_i_id FROM `stock` WHERE s_w_id = $w_id AND s_i_id = $i_id;
            )", std::move(params)),
                [this](TExecuteQueryResult result) -> TFuture<TOperationResult> {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    if (!parser.TryNextRow()) {
                        return RollbackThenFailOp(EErrorClass::Integrity, "stock update");
                    }
                    return ReadyOp(OkOp(1, 1));
                }));
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
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                DECLARE $number AS Int32;
                DECLARE $i_id AS Int32;
                DECLARE $supply_w_id AS Int32;
                DECLARE $quantity AS Int32;
                DECLARE $amount AS Decimal(22,9);
                DECLARE $dist_info AS Utf8;
                INSERT INTO `order_line` (ol_w_id, ol_d_id, ol_o_id, ol_number, ol_i_id,
                                          ol_amount, ol_supply_w_id, ol_quantity, ol_dist_info)
                VALUES ($w_id, $d_id, $o_id, $number, $i_id, $amount, $supply_w_id, $quantity, $dist_info);
            )", std::move(params), FinalCommitMode_),
                [](TExecuteQueryResult) { return OkOp(1, 1); }));
        }

        if (const auto* p = std::get_if<TApplyPaymentToLocation>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$amount").Decimal(Decimal(p->Amount)).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $amount AS Decimal(22,9);
                UPDATE `warehouse` SET w_ytd = w_ytd + $amount WHERE w_id = $w_id;
                UPDATE `district` SET d_ytd = d_ytd + $amount WHERE d_w_id = $w_id AND d_id = $d_id;
                SELECT w_name, w_street_1, w_street_2, w_city, w_state, w_zip
                  FROM `warehouse` WHERE w_id = $w_id;
                SELECT d_name, d_street_1, d_street_2, d_city, d_state, d_zip
                  FROM `district` WHERE d_w_id = $w_id AND d_id = $d_id;
            )", std::move(params)),
                [](TExecuteQueryResult result) {
                    TWarehouseDistrictInfo info;
                    NYdb::TResultSetParser wh(result.GetResultSet(0));
                    NYdb::TResultSetParser dist(result.GetResultSet(1));
                    if (!wh.TryNextRow() || !dist.TryNextRow()) {
                        return FailOp(EErrorClass::Integrity, "warehouse or district not found");
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
                    return OkOp(2, 2, std::move(info));
                }));
        }

        if (const auto* p = std::get_if<TGetCustomerData>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                SELECT c_data FROM `customer`
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
            )", std::move(params)),
                [](TExecuteQueryResult result) {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    if (!parser.TryNextRow()) {
                        return FailOp(EErrorClass::Integrity, "customer not found");
                    }
                    return OkOp(1, 1, ParseUtf8(parser, "c_data"));
                }));
        }

        if (const auto* p = std::get_if<TUpdateCustomerPayment>(&op)) {
            // Applied with the history insert in ExecuteFinalAndCommit, so both
            // statements share one script and c_data is set only for BC.
            PendingPaymentUpdate_ = *p;
            return ReadyOp(OkOp(1, 1));
        }

        if (const auto* p = std::get_if<TInsertPaymentHistory>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$h_w_id").Int32(p->PaymentWarehouseID).Build()
                .AddParam("$hist_id").Int64(NextHistoryId()).Build()
                .AddParam("$h_c_w_id").Int32(p->CustomerWarehouseID).Build()
                .AddParam("$h_c_d_id").Int32(p->CustomerDistrictID).Build()
                .AddParam("$h_c_id").Int32(p->CustomerID).Build()
                .AddParam("$h_d_id").Int32(p->PaymentDistrictID).Build()
                .AddParam("$h_amount").Decimal(Decimal(p->Amount)).Build()
                .AddParam("$h_data").Utf8(p->Data).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $h_w_id AS Int32;
                DECLARE $hist_id AS Int64;
                DECLARE $h_c_w_id AS Int32;
                DECLARE $h_c_d_id AS Int32;
                DECLARE $h_c_id AS Int32;
                DECLARE $h_d_id AS Int32;
                DECLARE $h_amount AS Decimal(22,9);
                DECLARE $h_data AS Utf8;
                INSERT INTO `history` (h_w_id, hist_id, h_c_w_id, h_c_d_id, h_c_id, h_d_id, h_date, h_amount, h_data)
                VALUES ($h_w_id, $hist_id, $h_c_w_id, $h_c_d_id, $h_c_id, $h_d_id,
                        CurrentUtcTimestamp(), $h_amount, $h_data);
            )", std::move(params), FinalCommitMode_),
                [](TExecuteQueryResult) { return OkOp(1, 1); }));
        }

        if (const auto* p = std::get_if<TGetLatestCustomerOrder>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                SELECT o_id, o_c_id, o_carrier_id, o_entry_d
                  FROM `oorder` VIEW `idx_order` AS idx
                 WHERE idx.o_w_id = $w_id AND idx.o_d_id = $d_id AND idx.o_c_id = $c_id
                 ORDER BY idx.o_id DESC LIMIT 1;
            )", std::move(params)),
                [](TExecuteQueryResult result) {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    if (!parser.TryNextRow()) {
                        return OkOp(0, 0);
                    }
                    TOrderHeader header;
                    header.OrderID = ParseInt32(parser, "o_id");
                    header.CustomerID = ParseInt32(parser, "o_c_id");
                    if (auto carrier = ParseOptionalInt32(parser, "o_carrier_id")) {
                        header.CarrierID = *carrier;
                    }
                    if (auto ts = ParseOptionalTimestamp(parser, "o_entry_d")) {
                        header.EntryDate = TimestampToString(*ts);
                    }
                    return OkOp(1, 1, std::move(header));
                }));
        }

        if (const auto* p = std::get_if<TGetOrderStatusLines>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$o_id").Int32(p->OrderID).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d
                  FROM `order_line`
                 WHERE ol_w_id = $w_id AND ol_d_id = $d_id AND ol_o_id = $o_id
                 ORDER BY ol_number;
            )", std::move(params), FinalCommitMode_),
                [](TExecuteQueryResult result) {
                    NYdb::TResultSetParser parser(result.GetResultSet(0));
                    std::vector<TOrderStatusLine> lines;
                    while (parser.TryNextRow()) {
                        TOrderStatusLine line;
                        line.ItemID = ParseInt32(parser, "ol_i_id");
                        line.SupplyWarehouseID = ParseInt32(parser, "ol_supply_w_id");
                        line.Quantity = ParseInt32(parser, "ol_quantity");
                        line.Amount = ParseMoney(parser, "ol_amount");
                        if (auto ts = ParseOptionalTimestamp(parser, "ol_delivery_d")) {
                            line.DeliveryDate = TimestampToString(*ts);
                        }
                        lines.push_back(std::move(line));
                    }
                    return OkOp(lines.size(), lines.size(), std::move(lines));
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

        if (const auto* p = std::get_if<TGetDeliveryOrderInfo>(&op)) {
            if (DeliveryPrefetch_.Loaded && DeliveryPrefetch_.WarehouseID == p->WarehouseID) {
                return ReadyOp(DeliveryInfoFromCache(p->DistrictID, p->OrderID));
            }
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$o_id").Int32(p->OrderID).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                SELECT o_c_id FROM `oorder`
                 WHERE o_w_id = $w_id AND o_d_id = $d_id AND o_id = $o_id;
                SELECT ol_amount FROM `order_line`
                 WHERE ol_w_id = $w_id AND ol_d_id = $d_id AND ol_o_id = $o_id;
            )", std::move(params)),
                [](TExecuteQueryResult result) {
                    NYdb::TResultSetParser order(result.GetResultSet(0));
                    if (!order.TryNextRow()) {
                        return FailOp(EErrorClass::Integrity, "order not found");
                    }
                    TDeliveryOrderInfo info;
                    info.CustomerID = ParseInt32(order, "o_c_id");
                    NYdb::TResultSetParser lines(result.GetResultSet(1));
                    int64_t totalCents = 0;
                    while (lines.TryNextRow()) {
                        totalCents += ParseMoney(lines, "ol_amount").Cents();
                        ++info.LineCount;
                    }
                    info.TotalAmount = TMoney::FromCents(totalCents);
                    return OkOp(1, 1, std::move(info));
                }));
        }

        if (const auto* p = std::get_if<TCompleteOrderDelivery>(&op)) {
            const uint64_t expectedLines = static_cast<uint64_t>(p->LineCount);
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$o_id").Int32(p->OrderID).Build()
                .AddParam("$carrier_id").Int32(p->CarrierID).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $o_id AS Int32;
                DECLARE $carrier_id AS Int32;
                SELECT no_o_id FROM `new_order`
                 WHERE no_w_id = $w_id AND no_d_id = $d_id AND no_o_id = $o_id;
                SELECT o_id FROM `oorder`
                 WHERE o_w_id = $w_id AND o_d_id = $d_id AND o_id = $o_id;
                SELECT COUNT(*) AS n FROM `order_line`
                 WHERE ol_w_id = $w_id AND ol_d_id = $d_id AND ol_o_id = $o_id;
                DELETE FROM `new_order`
                 WHERE no_w_id = $w_id AND no_d_id = $d_id AND no_o_id = $o_id;
                UPDATE `oorder`
                   SET o_carrier_id = $carrier_id
                 WHERE o_w_id = $w_id AND o_d_id = $d_id AND o_id = $o_id;
                UPDATE `order_line`
                   SET ol_delivery_d = CurrentUtcTimestamp()
                 WHERE ol_w_id = $w_id AND ol_d_id = $d_id AND ol_o_id = $o_id;
            )", std::move(params)),
                [this, expectedLines](TExecuteQueryResult result) -> TFuture<TOperationResult> {
                    NYdb::TResultSetParser lines(result.GetResultSet(2));
                    uint64_t lineCount = 0;
                    if (lines.TryNextRow()) {
                        lineCount = ParseCount(lines, "n");
                    }
                    if (auto error = DeliveryCardinalityError(
                            CountRows(result.GetResultSet(0)),
                            CountRows(result.GetResultSet(1)),
                            lineCount,
                            1,
                            expectedLines))
                    {
                        return RollbackThenFailOp(error->first, error->second);
                    }
                    return ReadyOp(OkOp(3, 3));
                }));
        }

        if (const auto* p = std::get_if<TApplyDeliveryToCustomer>(&op)) {
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(p->WarehouseID).Build()
                .AddParam("$d_id").Int32(p->DistrictID).Build()
                .AddParam("$c_id").Int32(p->CustomerID).Build()
                .AddParam("$amount").Decimal(Decimal(p->Amount)).Build()
                .Build();
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $c_id AS Int32;
                DECLARE $amount AS Decimal(22,9);
                UPDATE `customer`
                   SET c_balance = c_balance + $amount,
                       c_delivery_cnt = c_delivery_cnt + 1
                 WHERE c_w_id = $w_id AND c_d_id = $d_id AND c_id = $c_id;
            )", std::move(params), FinalCommitMode_),
                [](TExecuteQueryResult) { return OkOp(1, 1); }));
        }

        if (const auto* p = std::get_if<TCountRecentLowStock>(&op)) {
            const int warehouseId = p->WarehouseID;
            const int districtId = p->DistrictID;
            const int recentOrderCount = p->RecentOrderCount;
            const int threshold = p->Threshold;
            auto params = TParamsBuilder()
                .AddParam("$w_id").Int32(warehouseId).Build()
                .AddParam("$d_id").Int32(districtId).Build()
                .Build();
            // Two queries, same as ydb workload tpcc GetDistrictOrderId +
            // GetStockCount. YQL JOIN ON must be equality predicates only, so
            // the order-id window cannot live in ON (it belongs in WHERE with
            // bound $min_o_id / $max_o_id).
            return CatchOp(Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                SELECT d_next_o_id FROM `district`
                 WHERE d_w_id = $w_id AND d_id = $d_id;
            )", std::move(params)),
                [this, warehouseId, districtId, recentOrderCount, threshold](TExecuteQueryResult dist)
                    -> TFuture<TOperationResult> {
                    NYdb::TResultSetParser distParser(dist.GetResultSet(0));
                    if (!distParser.TryNextRow()) {
                        return ReadyOp(FailOp(EErrorClass::Integrity, "district not found"));
                    }
                    const int nextOid = ParseInt32(distParser, "d_next_o_id");
                    auto stockParams = TParamsBuilder()
                        .AddParam("$w_id").Int32(warehouseId).Build()
                        .AddParam("$d_id").Int32(districtId).Build()
                        .AddParam("$max_o_id").Int32(nextOid).Build()
                        .AddParam("$min_o_id").Int32(nextOid - recentOrderCount).Build()
                        .AddParam("$threshold").Int32(threshold).Build()
                        .Build();
                    return Then(
                        ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $d_id AS Int32;
                DECLARE $max_o_id AS Int32;
                DECLARE $min_o_id AS Int32;
                DECLARE $threshold AS Int32;
                SELECT COUNT(DISTINCT s.s_i_id) AS low_stock
                  FROM `order_line` AS ol
                  INNER JOIN `stock` AS s
                     ON s.s_i_id = ol.ol_i_id
                 WHERE ol.ol_w_id = $w_id AND ol.ol_d_id = $d_id
                   AND ol.ol_o_id < $max_o_id AND ol.ol_o_id >= $min_o_id
                   AND s.s_w_id = $w_id
                   AND s.s_quantity < $threshold;
            )", std::move(stockParams), FinalCommitMode_),
                        [](TExecuteQueryResult stock) {
                            NYdb::TResultSetParser stockParser(stock.GetResultSet(0));
                            int count = 0;
                            if (stockParser.TryNextRow()) {
                                count = static_cast<int>(ParseCount(stockParser, "low_stock"));
                            }
                            return OkOp(1, 1, count);
                        });
                }));
        }

        return ReadyOp(FailOp(EErrorClass::Permanent, "semantic op not yet bound in TYdbTpccTransaction"));
    }

TOperationResult TYdbTpccTransaction::OldestFromCache(int districtId) const {
    const int idx = districtId - DISTRICT_LOW_ID;
    if (idx < 0 || idx >= DISTRICT_COUNT) {
        return FailOp(EErrorClass::Permanent, "delivery district out of range");
    }
    if (!DeliveryPrefetch_.OldestOrderId[idx]) {
        return OkOp(0, 0, 0);
    }
    return OkOp(1, 1, *DeliveryPrefetch_.OldestOrderId[idx]);
}

TOperationResult TYdbTpccTransaction::DeliveryInfoFromCache(int districtId, int orderId) const {
    const int idx = districtId - DISTRICT_LOW_ID;
    if (idx < 0 || idx >= DISTRICT_COUNT || !DeliveryPrefetch_.Info[idx]) {
        return FailOp(EErrorClass::Integrity, "order not found");
    }
    if (DeliveryPrefetch_.OldestOrderId[idx] && *DeliveryPrefetch_.OldestOrderId[idx] != orderId) {
        return FailOp(EErrorClass::Integrity, "order not found");
    }
    return OkOp(1, 1, *DeliveryPrefetch_.Info[idx]);
}

TFuture<TOperationResult> TYdbTpccTransaction::EnsureDeliveryPrefetch(int warehouseId) {
    if (DeliveryPrefetch_.Loaded && DeliveryPrefetch_.WarehouseID == warehouseId) {
        return ReadyOp(OkOp(1, 1));
    }
    DeliveryPrefetch_ = {};
    DeliveryPrefetch_.WarehouseID = warehouseId;
    auto params = TParamsBuilder().AddParam("$w_id").Int32(warehouseId).Build().Build();
    auto loaded = Then(
        ExecQuery(Prefix(Path_) + OldestNewOrdersSql(), std::move(params)),
        [this, warehouseId](TExecuteQueryResult oldest) -> TFuture<TOperationResult> {
            const auto& sets = oldest.GetResultSets();
            if (sets.size() != static_cast<size_t>(DISTRICT_COUNT)) {
                return RollbackThenFailOp(
                    EErrorClass::Integrity, "delivery oldest prefetch size mismatch");
            }
            std::vector<std::pair<int, int>> found;
            found.reserve(DISTRICT_COUNT);
            for (int districtId = DISTRICT_LOW_ID; districtId <= DISTRICT_HIGH_ID; ++districtId) {
                const int idx = districtId - DISTRICT_LOW_ID;
                NYdb::TResultSetParser parser(sets[static_cast<size_t>(idx)]);
                if (!parser.TryNextRow()) {
                    continue;
                }
                const int orderId = ParseInt32(parser, "no_o_id");
                DeliveryPrefetch_.OldestOrderId[idx] = orderId;
                found.emplace_back(districtId, orderId);
            }
            if (found.empty()) {
                DeliveryPrefetch_.Loaded = true;
                return ReadyOp(OkOp(0, 0));
            }
            TParamsBuilder keys;
            auto& list = keys.AddParam("$keys").BeginList();
            for (const auto& key : found) {
                list.AddListItem()
                    .BeginTuple()
                    .AddElement().Int32(key.first)
                    .AddElement().Int32(key.second)
                    .EndTuple();
            }
            auto built = list.EndList().Build()
                .AddParam("$w_id").Int32(warehouseId).Build()
                .Build();
            const size_t foundCount = found.size();
            return Then(
                ExecQuery(Prefix(Path_) + R"(
                DECLARE $w_id AS Int32;
                DECLARE $keys AS List<Tuple<Int32, Int32>>;
                SELECT o_d_id, o_id, o_c_id FROM `oorder`
                 WHERE o_w_id = $w_id AND (o_d_id, o_id) IN $keys;
                SELECT ol_d_id, ol_o_id, ol_amount FROM `order_line`
                 WHERE ol_w_id = $w_id AND (ol_d_id, ol_o_id) IN $keys;
            )", std::move(built)),
                [this, foundCount](TExecuteQueryResult info) -> TFuture<TOperationResult> {
                    if (info.GetResultSets().size() < 2) {
                        return RollbackThenFailOp(
                            EErrorClass::Integrity, "delivery info prefetch size mismatch");
                    }
                    NYdb::TResultSetParser orders(info.GetResultSet(0));
                    while (orders.TryNextRow()) {
                        const int districtId = ParseInt32(orders, "o_d_id");
                        const int idx = districtId - DISTRICT_LOW_ID;
                        if (idx < 0 || idx >= DISTRICT_COUNT) {
                            continue;
                        }
                        TDeliveryOrderInfo row;
                        row.CustomerID = ParseInt32(orders, "o_c_id");
                        DeliveryPrefetch_.Info[idx] = row;
                    }
                    NYdb::TResultSetParser lines(info.GetResultSet(1));
                    while (lines.TryNextRow()) {
                        const int districtId = ParseInt32(lines, "ol_d_id");
                        const int idx = districtId - DISTRICT_LOW_ID;
                        if (idx < 0 || idx >= DISTRICT_COUNT || !DeliveryPrefetch_.Info[idx]) {
                            continue;
                        }
                        auto& row = *DeliveryPrefetch_.Info[idx];
                        row.TotalAmount += ParseMoney(lines, "ol_amount");
                        row.LineCount += 1;
                    }
                    for (int districtId = DISTRICT_LOW_ID; districtId <= DISTRICT_HIGH_ID; ++districtId) {
                        const int idx = districtId - DISTRICT_LOW_ID;
                        if (DeliveryPrefetch_.OldestOrderId[idx] && !DeliveryPrefetch_.Info[idx]) {
                            return RollbackThenFailOp(EErrorClass::Integrity, "order not found");
                        }
                    }
                    DeliveryPrefetch_.Loaded = true;
                    return ReadyOp(OkOp(foundCount, foundCount));
                });
        });
    // A query error is a value here. Roll the open transaction back unless a
    // cardinality miss already did.
    return Then(
        CatchOp(std::move(loaded)),
        [this](TOperationResult result) -> TFuture<TOperationResult> {
            if (!result.Ok && !Terminal_) {
                return Then(Rollback(), [result = std::move(result)](TCommitResult) {
                    return result;
                });
            }
            return ReadyOp(std::move(result));
        });
}

TYdbTpccSession::TYdbTpccSession(TYdbConnection& connection, std::string path)
    : Connection_(connection)
    , Path_(std::move(path))
{}

TFuture<std::unique_ptr<ITpccTransaction>> TYdbTpccSession::Begin(EIsolationLevel isolation) {
    const TTxSettings txSettings = YdbTxSettingsForIsolation(isolation);
    return Then(
        BridgeYdbFuture(Connection_.QueryClient().GetSession(MakeYdbCreateSessionSettings())),
        [this, txSettings](NYdb::NQuery::TCreateSessionResult result) {
            if (!result.IsSuccess()) {
                throw NYdb::NStatusHelpers::TYdbErrorException(std::move(result));
            }
            return std::unique_ptr<ITpccTransaction>(
                std::make_unique<TYdbTpccTransaction>(result.GetSession(), Path_, txSettings));
        });
}

TYdbSessionFactory::TYdbSessionFactory(TYdbConnection& connection)
    : Connection_(connection)
{}

std::unique_ptr<ITpccSession> TYdbSessionFactory::CreateSession() {
    return std::make_unique<TYdbTpccSession>(
        Connection_, Connection_.AbsolutePathPrefix());
}

} // namespace NTpcc

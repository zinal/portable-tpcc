#include <gtest/gtest.h>

#include <debug_probe.h>
#include <error_classifier.h>
#include <future.h>
#include <ops.h>
#include <session.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

using namespace NTpcc;
using Json = nlohmann::json;

namespace {

class TPermanentClassifier final : public IErrorClassifier {
public:
    EErrorClass Classify(std::string_view, std::string_view) const override {
        return EErrorClass::Permanent;
    }
};

TFuture<TOperationResult> MakeFailedOp(std::string message) {
    TPromise<TOperationResult> p;
    TOperationResult r;
    r.Ok = false;
    r.ErrorClass = EErrorClass::Permanent;
    r.Message = std::move(message);
    p.SetValue(std::move(r));
    return p.GetFuture();
}

TFuture<TBatchResult> MakeFailedBatch(std::string message) {
    TPromise<TBatchResult> p;
    TBatchResult r;
    r.Ok = false;
    r.ErrorClass = EErrorClass::Permanent;
    r.Message = std::move(message);
    p.SetValue(std::move(r));
    return p.GetFuture();
}

TFuture<TFinalCommitResult> MakeFailedFinal(std::string message) {
    TPromise<TFinalCommitResult> p;
    TFinalCommitResult r;
    r.Operation.Ok = false;
    r.Operation.ErrorClass = EErrorClass::Permanent;
    r.Operation.Message = message;
    r.Commit.Outcome = ECommitOutcome::RolledBack;
    r.Commit.ErrorClass = EErrorClass::Permanent;
    r.Commit.Message = std::move(message);
    p.SetValue(std::move(r));
    return p.GetFuture();
}

TFuture<TCommitResult> MakeRolledBack(std::string message) {
    TPromise<TCommitResult> p;
    TCommitResult r;
    r.Outcome = ECommitOutcome::RolledBack;
    r.ErrorClass = EErrorClass::Permanent;
    r.Message = std::move(message);
    p.SetValue(std::move(r));
    return p.GetFuture();
}

class TFailingTx final : public ITpccTransaction {
public:
    TFuture<TOperationResult> Execute(const TSemanticOp&) override {
        return MakeFailedOp("mock execute failed");
    }
    TFuture<TBatchResult> ExecuteBatch(const std::vector<TSemanticOp>&) override {
        return MakeFailedBatch("mock batch failed");
    }
    TFuture<TFinalCommitResult> ExecuteFinalAndCommit(const TSemanticOp&) override {
        return MakeFailedFinal("mock final failed");
    }
    TFuture<TCommitResult> Commit() override {
        return MakeRolledBack("mock commit failed");
    }
    TFuture<TCommitResult> Rollback() override {
        return MakeRolledBack("mock rollback");
    }
    TFuture<TCommitResult> Cancel() override {
        return MakeRolledBack("mock cancel");
    }
};

class TFailingSession final : public ITpccSession {
public:
    TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel) override {
        TPromise<std::unique_ptr<ITpccTransaction>> p;
        p.SetValue(std::make_unique<TFailingTx>());
        return p.GetFuture();
    }
};

class TFailingFactory final : public ISessionFactory {
public:
    std::unique_ptr<ITpccSession> CreateSession() override {
        return std::make_unique<TFailingSession>();
    }
};

} // anonymous

TEST(DebugProbe, ComputeDurationStatsEmpty) {
    const auto s = ComputeDurationStats({});
    EXPECT_EQ(s.First, 0);
    EXPECT_EQ(s.Min, 0);
    EXPECT_EQ(s.Max, 0);
    EXPECT_EQ(s.Avg, 0);
    EXPECT_FALSE(s.RestAvg.has_value());
}

TEST(DebugProbe, ComputeDurationStatsFirstVsRest) {
    const auto s = ComputeDurationStats({50, 10, 20, 10});
    EXPECT_EQ(s.First, 50);
    EXPECT_EQ(s.Min, 10);
    EXPECT_EQ(s.Max, 50);
    EXPECT_EQ(s.Avg, 22);
    ASSERT_TRUE(s.RestAvg.has_value());
    EXPECT_EQ(*s.RestAvg, 13);
}

TEST(DebugProbe, JsonNames) {
    EXPECT_STREQ(TransactionTypeJsonName(ETransactionType::NewOrder), "new_order");
    EXPECT_STREQ(TransactionTypeJsonName(ETransactionType::Payment), "payment");
    EXPECT_STREQ(TransactionTypeJsonName(ETransactionType::StockLevel), "stock_level");
}

TEST(DebugProbe, ReportJsonRoundTripShape) {
    TDebugReport report;
    report.RunId = "run-1";
    report.Instance = "debug-0";
    report.Repeats = 2;
    TDebugTxReport tx;
    tx.Type = "new_order";
    tx.Ok = 2;
    tx.Attempts.push_back(TDebugAttempt{1, "ok", 100, 90, ""});
    tx.Attempts.push_back(TDebugAttempt{2, "ok", 40, 35, ""});
    tx.DurationUs = ComputeDurationStats({100, 40});
    tx.LatencyPureUs = ComputeDurationStats({90, 35});
    report.Transactions.push_back(tx);

    const auto parsed = Json::parse(DebugReportToJson(report));
    EXPECT_EQ(parsed.at("schema_version"), 1);
    EXPECT_EQ(parsed.at("role"), "debug");
    EXPECT_EQ(parsed.at("repeats"), 2);
    EXPECT_TRUE(parsed.at("ok").get<bool>());
    EXPECT_EQ(parsed.at("transactions").size(), 1u);
    EXPECT_EQ(parsed.at("transactions")[0].at("duration_us").at("first"), 100);
    EXPECT_EQ(parsed.at("transactions")[0].at("duration_us").at("rest_avg"), 40);
}

TEST(DebugProbe, SequentialProbeRecordsEachType) {
    TFailingFactory factory;
    TPermanentClassifier classifier;
    TDebugProbeRequest req;
    req.SessionFactory = &factory;
    req.ErrorClassifier = &classifier;
    req.WarehouseID = 1;
    req.WarehouseCount = 1;
    req.DistrictID = 1;
    req.Repeats = 2;
    req.RunId = "ut";
    req.Instance = "debug-ut";

    const auto report = RunDebugProbe(req);
    EXPECT_FALSE(report.Ok);
    ASSERT_EQ(report.Transactions.size(), TRANSACTION_TYPE_COUNT);
    EXPECT_EQ(report.Transactions[0].Type, "new_order");
    EXPECT_EQ(report.Transactions[1].Type, "payment");
    EXPECT_EQ(report.Transactions[2].Type, "order_status");
    EXPECT_EQ(report.Transactions[3].Type, "delivery");
    EXPECT_EQ(report.Transactions[4].Type, "stock_level");
    for (const auto& tx : report.Transactions) {
        EXPECT_EQ(tx.Attempts.size(), 2u);
        EXPECT_EQ(tx.Failed, 2);
        EXPECT_EQ(tx.Ok, 0);
        EXPECT_GT(tx.Attempts[0].DurationUs, 0);
    }
    EXPECT_EQ(report.FailedCount(), static_cast<int>(TRANSACTION_TYPE_COUNT) * 2);
}

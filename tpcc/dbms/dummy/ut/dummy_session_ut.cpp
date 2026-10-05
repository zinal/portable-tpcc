#include <gtest/gtest.h>

#include <constants.h>
#include <debug_probe.h>
#include <dummy_error_classifier.h>
#include <dummy_session.h>
#include <ops.h>

#include <chrono>
#include <variant>

using namespace NTpcc;

namespace {

TOperationResult ReadyExecute(ITpccTransaction& tx, const TSemanticOp& op) {
    return tx.Execute(op).Get();
}

} // namespace

TEST(DummySession, CustomerAndUnusedItemPayloads) {
    TDummySessionFactory factory(TDummyDelayConfig{});
    auto session = factory.CreateSession();
    auto tx = session->Begin(EIsolationLevel::RepeatableRead).Get();

    auto customer = ReadyExecute(*tx, TGetCustomerById{1, 1, 10});
    ASSERT_TRUE(customer.Ok);
    const auto& row = std::get<TCustomerRow>(customer.Payload);
    EXPECT_EQ(row.CustomerID, 10);
    EXPECT_EQ(row.Credit, "BC");

    auto missing = ReadyExecute(*tx, TGetItems{std::vector<int>{INVALID_ITEM_ID}});
    EXPECT_FALSE(missing.Ok);
    EXPECT_EQ(missing.ErrorClass, EErrorClass::Integrity);

    auto items = ReadyExecute(*tx, TGetItems{std::vector<int>{1, 2}});
    ASSERT_TRUE(items.Ok);
    EXPECT_EQ(std::get<std::vector<TItemRow>>(items.Payload).size(), 2u);

    auto commit = tx->Commit().Get();
    EXPECT_EQ(commit.Outcome, ECommitOutcome::Committed);
}

TEST(DummySession, DelayAppliedOnCommit) {
    TDummyDelayConfig delay;
    delay.MinUs = 2000;
    delay.MaxUs = 2000;
    TDummySessionFactory factory(delay, 1);
    auto session = factory.CreateSession();
    auto tx = session->Begin(EIsolationLevel::RepeatableRead).Get();
    const auto start = std::chrono::steady_clock::now();
    auto commit = tx->Commit().Get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_EQ(commit.Outcome, ECommitOutcome::Committed);
    EXPECT_GE(elapsed.count(), 1000);
}

TEST(DummySession, DebugProbeExercisesAllTransactionTypes) {
    TDummySessionFactory factory(TDummyDelayConfig{});
    TDummyErrorClassifier classifier;
    TDebugProbeRequest req;
    req.SessionFactory = &factory;
    req.ErrorClassifier = &classifier;
    req.WarehouseID = 1;
    req.WarehouseCount = 1;
    req.DistrictID = 1;
    req.Repeats = 1;
    const auto report = RunDebugProbe(req);
    EXPECT_TRUE(report.Ok) << DebugReportToJson(report);
    EXPECT_EQ(report.Transactions.size(), 5u);
    for (const auto& tx : report.Transactions) {
        EXPECT_EQ(tx.Failed, 0) << tx.Type;
        EXPECT_GE(tx.Ok + tx.UserAborted, 1) << tx.Type;
    }
}

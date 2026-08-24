#include <gtest/gtest.h>

#include <context.h>
#include <ops.h>
#include <workflow_util.h>

using namespace NTpcc;

namespace {

TCustomerRow MakeCustomer(int id, std::string first) {
    TCustomerRow c;
    c.CustomerID = id;
    c.First = std::move(first);
    return c;
}

} // namespace

TEST(SelectCustomerByLastNameMedian, Empty) {
    EXPECT_FALSE(SelectCustomerByLastNameMedian({}).has_value());
}

TEST(SelectCustomerByLastNameMedian, SingleAndEvenOdd) {
    {
        auto c = SelectCustomerByLastNameMedian({MakeCustomer(1, "A")});
        ASSERT_TRUE(c);
        EXPECT_EQ(c->CustomerID, 1);
    }
    {
        // n=2 → position 1 (1-based) → index 0
        auto c = SelectCustomerByLastNameMedian({MakeCustomer(1, "A"), MakeCustomer(2, "B")});
        ASSERT_TRUE(c);
        EXPECT_EQ(c->CustomerID, 1);
    }
    {
        // n=3 → position 2 → index 1
        auto c = SelectCustomerByLastNameMedian({
            MakeCustomer(1, "A"), MakeCustomer(2, "B"), MakeCustomer(3, "C")});
        ASSERT_TRUE(c);
        EXPECT_EQ(c->CustomerID, 2);
    }
}

TEST(ClassifiedError, CarriesClass) {
    TClassifiedError err(EErrorClass::RetryableAbort, "40001", "serialization");
    EXPECT_EQ(err.Class, EErrorClass::RetryableAbort);
    EXPECT_EQ(err.NativeCode, "40001");
}

TEST(IsExpectedItemNotFound, AcceptsOnlyIntegrityFailure) {
    TOperationResult missing;
    missing.Ok = false;
    missing.ErrorClass = EErrorClass::Integrity;
    missing.Message = "item not found";
    EXPECT_TRUE(IsExpectedItemNotFound(missing));

    TOperationResult ok;
    ok.Ok = true;
    ok.ErrorClass = EErrorClass::Integrity;
    EXPECT_FALSE(IsExpectedItemNotFound(ok));

    TOperationResult permanent;
    permanent.Ok = false;
    permanent.ErrorClass = EErrorClass::Permanent;
    permanent.Message = "permission denied";
    EXPECT_FALSE(IsExpectedItemNotFound(permanent));

    TOperationResult cancelled;
    cancelled.Ok = false;
    cancelled.ErrorClass = EErrorClass::Cancelled;
    EXPECT_FALSE(IsExpectedItemNotFound(cancelled));
}

TEST(ThrowIfRollbackFailed, AcceptsConfirmedRollbackOnly) {
    TCommitResult rolledBack;
    rolledBack.Outcome = ECommitOutcome::RolledBack;
    EXPECT_NO_THROW(ThrowIfRollbackFailed(rolledBack));

    TCommitResult unknown;
    unknown.Outcome = ECommitOutcome::OutcomeUnknown;
    unknown.ErrorClass = EErrorClass::Permanent;
    unknown.Message = "rollback failed";
    try {
        ThrowIfRollbackFailed(unknown);
        FAIL() << "expected TClassifiedError";
    } catch (const TClassifiedError& ex) {
        EXPECT_EQ(ex.Class, EErrorClass::Permanent);
        EXPECT_EQ(std::string(ex.what()), "rollback failed");
    }

    TCommitResult committed;
    committed.Outcome = ECommitOutcome::Committed;
    EXPECT_THROW(ThrowIfRollbackFailed(committed), TClassifiedError);
}

TEST(ThrowIfBatchRetryable, OkDoesNotThrow) {
    TBatchResult batch;
    batch.Ok = true;
    EXPECT_NO_THROW(ThrowIfBatchRetryable(batch));
}

TEST(ThrowIfBatchRetryable, RetryableAndAmbiguousThrow) {
    TBatchResult retryable;
    retryable.Ok = false;
    retryable.ErrorClass = EErrorClass::RetryableAbort;
    retryable.NativeCode = "ABORTED";
    retryable.Message = "transaction aborted";
    try {
        ThrowIfBatchRetryable(retryable);
        FAIL() << "expected TClassifiedError";
    } catch (const TClassifiedError& ex) {
        EXPECT_EQ(ex.Class, EErrorClass::RetryableAbort);
        EXPECT_EQ(ex.NativeCode, "ABORTED");
        EXPECT_EQ(std::string(ex.what()), "transaction aborted");
    }

    TBatchResult ambiguous;
    ambiguous.Ok = false;
    ambiguous.ErrorClass = EErrorClass::AmbiguousCommit;
    ambiguous.Message = "commit status unknown";
    EXPECT_THROW(ThrowIfBatchRetryable(ambiguous), TClassifiedError);
}

TEST(ThrowIfBatchRetryable, PermanentDoesNotThrow) {
    TBatchResult permanent;
    permanent.Ok = false;
    permanent.ErrorClass = EErrorClass::Permanent;
    permanent.Message = "syntax error";
    EXPECT_NO_THROW(ThrowIfBatchRetryable(permanent));
}

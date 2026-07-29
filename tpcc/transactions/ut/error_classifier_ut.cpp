#include <gtest/gtest.h>

#include <error_classifier.h>

using namespace NTpcc;

TEST(ClassifySqlState, SerializationAndDeadlock) {
    EXPECT_EQ(ClassifySqlState("40001"), EErrorClass::RetryableAbort);
    EXPECT_EQ(ClassifySqlState("40P01"), EErrorClass::RetryableAbort);
    EXPECT_EQ(ClassifySqlState("40000"), EErrorClass::RetryableAbort);
}

TEST(ClassifySqlState, ConnectionNotCommitted) {
    EXPECT_EQ(ClassifySqlState("08000"), EErrorClass::NotCommitted);
    EXPECT_EQ(ClassifySqlState("08006"), EErrorClass::NotCommitted);
}

TEST(ClassifySqlState, IntegrityConstraints) {
    EXPECT_EQ(ClassifySqlState("23505"), EErrorClass::Integrity);
    EXPECT_EQ(ClassifySqlState("23000"), EErrorClass::Integrity);
}

TEST(ClassifySqlState, CancelledBeforeClass57) {
    EXPECT_EQ(ClassifySqlState("57014"), EErrorClass::Cancelled);
    EXPECT_EQ(ClassifySqlState("57P01"), EErrorClass::Permanent);
}

TEST(ClassifySqlState, PermanentFallback) {
    EXPECT_EQ(ClassifySqlState(""), EErrorClass::Permanent);
    EXPECT_EQ(ClassifySqlState("42"), EErrorClass::Permanent);
    EXPECT_EQ(ClassifySqlState("42601"), EErrorClass::Permanent);
}

TEST(RetryPolicy, BlindRetryRules) {
    EXPECT_TRUE(IsRetryable(EErrorClass::RetryableAbort));
    EXPECT_TRUE(IsRetryable(EErrorClass::NotCommitted));
    EXPECT_FALSE(IsRetryable(EErrorClass::AmbiguousCommit));
    EXPECT_FALSE(IsRetryable(EErrorClass::Integrity));

    EXPECT_TRUE(MayBlindRetry(EErrorClass::RetryableAbort));
    EXPECT_TRUE(MayBlindRetry(EErrorClass::NotCommitted));
    EXPECT_FALSE(MayBlindRetry(EErrorClass::AmbiguousCommit));
    EXPECT_FALSE(MayBlindRetry(EErrorClass::Permanent));
    EXPECT_FALSE(MayBlindRetry(EErrorClass::Integrity));
    EXPECT_FALSE(MayBlindRetry(EErrorClass::Cancelled));
}

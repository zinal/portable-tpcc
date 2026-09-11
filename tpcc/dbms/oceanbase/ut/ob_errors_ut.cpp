#include <gtest/gtest.h>

#include <ob_connection.h>
#include <ob_error_classifier.h>
#include <ob_errors.h>

#include <stdexcept>

using namespace NTpcc;

TEST(ObErrors, ClassifyConnectionLostCodes) {
    EXPECT_EQ(ClassifyDbError(2006), EObDbErrorKind::ConnectionLost);
    EXPECT_EQ(ClassifyDbError(2013), EObDbErrorKind::ConnectionLost);
    EXPECT_EQ(ClassifyDbError(2014), EObDbErrorKind::ConnectionLost);
    EXPECT_EQ(ClassifyDbError(2027), EObDbErrorKind::ConnectionLost);
    EXPECT_EQ(ClassifyDbError(2055), EObDbErrorKind::ConnectionLost);
    EXPECT_EQ(ClassifyDbError(2002), EObDbErrorKind::ConnectionLost);
    EXPECT_EQ(ClassifyDbError(2003), EObDbErrorKind::ConnectionLost);
    EXPECT_EQ(ClassifyDbError(1213), EObDbErrorKind::Deadlock);
    EXPECT_EQ(ClassifyDbError(1205), EObDbErrorKind::LockWaitTimeout);
}

TEST(ObQuoteSqlString, QuotesSafeLiterals) {
    EXPECT_EQ(QuoteSqlString("tpcc"), "'tpcc'");
    EXPECT_EQ(QuoteSqlString("customer"), "'customer'");
    EXPECT_THROW(QuoteSqlString(""), std::invalid_argument);
    EXPECT_THROW(QuoteSqlString("o'brian"), std::invalid_argument);
}

TEST(ObErrorClassifier, LostConnectionDuringPrepareIsNotCommitted) {
    TObErrorClassifier classifier;
    TObDbError err(2013, "mysql_stmt_prepare failed: [2013] Lost connection to MySQL server during query");
    EXPECT_EQ(classifier.ClassifyException(err), EErrorClass::NotCommitted);
    EXPECT_TRUE(IsConnectionLostCode(2013));
}

TEST(ObErrorClassifier, MalformedPacketIsNotCommitted) {
    TObErrorClassifier classifier;
    TObDbError err(2027, "mysql_real_connect failed: [2027] received malformed packet");
    EXPECT_EQ(classifier.ClassifyException(err), EErrorClass::NotCommitted);
    EXPECT_EQ(classifier.ClassifyCommitException(err), EErrorClass::AmbiguousCommit);
    EXPECT_EQ(classifier.Classify("2027"), EErrorClass::NotCommitted);
}

TEST(ObErrors, TenantMemoryLimitIsNotConnectionLost) {
    EXPECT_EQ(ClassifyDbError(4013), EObDbErrorKind::TenantMemoryLimit);
    EXPECT_EQ(ClassifyDbError(-4013), EObDbErrorKind::TenantMemoryLimit);
    EXPECT_EQ(
        ClassifyDbError(2013, "No memory or reach tenant memory limit"),
        EObDbErrorKind::TenantMemoryLimit);
    EXPECT_FALSE(IsConnectionLostCode(4013));
    EXPECT_FALSE(IsRetryableTxError(EObDbErrorKind::TenantMemoryLimit));
    EXPECT_EQ(PreferObNativeCode(2013, 4013, "Lost connection to MySQL server during query"), 4013);
    EXPECT_EQ(
        PreferObNativeCode(2013, 0, "mysql_stmt_prepare failed: No memory or reach tenant memory limit"),
        4013);
}

TEST(ObErrorClassifier, TenantMemoryLimitIsPermanent) {
    TObErrorClassifier classifier;
    TObDbError err(
        4013,
        "mysql_stmt_prepare failed: [4013] No memory or reach tenant memory limit");
    EXPECT_EQ(classifier.ClassifyException(err), EErrorClass::Permanent);
    EXPECT_EQ(classifier.ClassifyCommitException(err), EErrorClass::Permanent);
    EXPECT_EQ(classifier.Classify("4013", "No memory or reach tenant memory limit"),
              EErrorClass::Permanent);
    TObDbError wrapped(
        2013,
        "mysql_stmt_prepare failed: [2013] No memory or reach tenant memory limit");
    EXPECT_EQ(classifier.ClassifyException(wrapped), EErrorClass::Permanent);
    EXPECT_EQ(wrapped.Kind(), EObDbErrorKind::TenantMemoryLimit);
}

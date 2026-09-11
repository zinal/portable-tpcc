#include <gtest/gtest.h>

#include <ob_error_classifier.h>
#include <ob_errors.h>

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

TEST(ObErrorClassifier, MalformedPacketIsNotCommitted) {
    TObErrorClassifier classifier;
    TObDbError err(2027, "mysql_real_connect failed: [2027] received malformed packet");
    EXPECT_EQ(classifier.ClassifyException(err), EErrorClass::NotCommitted);
    EXPECT_EQ(classifier.ClassifyCommitException(err), EErrorClass::AmbiguousCommit);
    EXPECT_EQ(classifier.Classify("2027"), EErrorClass::NotCommitted);
}

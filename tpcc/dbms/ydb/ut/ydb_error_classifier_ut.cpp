#include <gtest/gtest.h>

#include <ydb_error_classifier.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/library/issue/yql_issue.h>

using namespace NTpcc;

namespace {

NYdb::TStatus StatusOf(NYdb::EStatus code, const char* issue = nullptr) {
    NYdb::NIssue::TIssues issues;
    if (issue != nullptr) {
        issues.AddIssue(NYdb::NIssue::TIssue(issue));
    }
    return NYdb::TStatus(code, std::move(issues));
}

} // anonymous

TEST(YdbStatusCodeOf, PrintsSymbolicNameAndNumericCode) {
    EXPECT_EQ(YdbStatusCodeOf(NYdb::EStatus::OVERLOADED), "OVERLOADED (400060)");
    EXPECT_EQ(YdbStatusCodeOf(NYdb::EStatus::ABORTED), "ABORTED (400040)");
    EXPECT_EQ(YdbStatusCodeOf(NYdb::EStatus::UNAVAILABLE), "UNAVAILABLE (400050)");
    EXPECT_EQ(YdbStatusCodeOf(NYdb::EStatus::TRANSPORT_UNAVAILABLE), "TRANSPORT_UNAVAILABLE (401010)");
    EXPECT_EQ(YdbStatusCodeOf(NYdb::EStatus::CLIENT_LIMITS_REACHED), "CLIENT_LIMITS_REACHED (402020)");
}

TEST(YdbIssuesToString, PrefixesIssuesWithStatusCode) {
    const auto text = YdbIssuesToString(StatusOf(NYdb::EStatus::OVERLOADED, "shard overloaded"));
    EXPECT_EQ(text.find("OVERLOADED (400060)"), 0u);
    EXPECT_NE(text.find("shard overloaded"), std::string::npos);
}

TEST(YdbIssuesToString, StatusOnlyWhenNoIssues) {
    EXPECT_EQ(YdbIssuesToString(StatusOf(NYdb::EStatus::ABORTED)), "ABORTED (400040)");
}

TEST(YdbErrorClassifier, ClassifiesNumericAndSymbolicNativeCodes) {
    const TYdbErrorClassifier classifier;
    EXPECT_EQ(classifier.Classify("400060"), EErrorClass::RetryableAbort);
    EXPECT_EQ(classifier.Classify("OVERLOADED"), EErrorClass::RetryableAbort);
    EXPECT_EQ(classifier.Classify("OVERLOADED (400060)"), EErrorClass::RetryableAbort);
    EXPECT_EQ(classifier.Classify("ABORTED (400040)"), EErrorClass::RetryableAbort);
    EXPECT_EQ(classifier.Classify("400170"), EErrorClass::AmbiguousCommit);
    EXPECT_EQ(classifier.Classify("UNDETERMINED"), EErrorClass::AmbiguousCommit);
    EXPECT_EQ(classifier.Classify("PRECONDITION_FAILED (400120)"), EErrorClass::Integrity);
    EXPECT_EQ(classifier.Classify("400010"), EErrorClass::Permanent);
}

TEST(YdbErrorClassifier, ClassifiesMessageWhenNativeCodeMissing) {
    const TYdbErrorClassifier classifier;
    EXPECT_EQ(classifier.Classify({}, "query failed: OVERLOADED"), EErrorClass::RetryableAbort);
    EXPECT_EQ(classifier.Classify({}, "TRANSPORT_UNAVAILABLE"), EErrorClass::NotCommitted);
    EXPECT_EQ(classifier.Classify({}, "CLIENT_INTERNAL_ERROR"), EErrorClass::NotCommitted);
    EXPECT_EQ(classifier.Classify({}, "syntax error"), EErrorClass::Permanent);
}

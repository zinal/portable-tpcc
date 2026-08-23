#include <gtest/gtest.h>

#include <arrow_upsert.h>
#include <ydb_error_classifier.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/library/issue/yql_issue.h>

using namespace NTpcc;

namespace {

NYdb::TStatus StatusOf(NYdb::EStatus code) {
    return NYdb::TStatus(code, NYdb::NIssue::TIssues{});
}

} // anonymous

TEST(YdbBulkUpsertRetry, RetriesTimeoutAndOverload) {
    EXPECT_TRUE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::TIMEOUT)));
    EXPECT_TRUE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::OVERLOADED)));
    EXPECT_TRUE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::UNAVAILABLE)));
    EXPECT_TRUE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::ABORTED)));
    EXPECT_TRUE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::TRANSPORT_UNAVAILABLE)));
    EXPECT_TRUE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::UNDETERMINED)));
}

TEST(YdbBulkUpsertRetry, DoesNotRetryPermanentOrCancel) {
    EXPECT_FALSE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::SUCCESS)));
    EXPECT_FALSE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::BAD_REQUEST)));
    EXPECT_FALSE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::SCHEME_ERROR)));
    EXPECT_FALSE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::CANCELLED)));
    EXPECT_FALSE(ShouldRetryYdbBulkUpsert(StatusOf(NYdb::EStatus::PRECONDITION_FAILED)));
}

TEST(YdbBulkUpsertRetry, TimeoutClassifiesAsRetryableAbort) {
    EXPECT_EQ(
        TYdbErrorClassifier{}.ClassifyStatus(StatusOf(NYdb::EStatus::TIMEOUT)),
        EErrorClass::RetryableAbort);
}

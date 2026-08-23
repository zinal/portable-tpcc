#include <ydb_driver.h>

#include <gtest/gtest.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/resources/ydb_resources.h>

using namespace NTpcc;

TEST(YdbCreateSessionSettings, EnablesSessionBalancerCapability) {
    const auto settings = MakeYdbCreateSessionSettings();
    ASSERT_EQ(settings.Header_.size(), 1u);
    EXPECT_EQ(settings.Header_[0].first, NYdb::YDB_CLIENT_CAPABILITIES);
    EXPECT_EQ(settings.Header_[0].second, NYdb::YDB_CLIENT_CAPABILITY_SESSION_BALANCER);
}

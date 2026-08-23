#include <ydb_driver.h>

#include <gtest/gtest.h>

using namespace NTpcc;

TEST(YdbNodeEndpoint, FormatsHostPort) {
    EXPECT_EQ(FormatYdbNodeHostPort("ydb-1.example.net", 2135), "ydb-1.example.net:2135");
    EXPECT_EQ(FormatYdbNodeHostPort("127.0.0.1", 2136), "127.0.0.1:2136");
}

TEST(YdbNodeEndpoint, WrapsIpv6) {
    EXPECT_EQ(FormatYdbNodeHostPort("::1", 2135), "[::1]:2135");
    EXPECT_EQ(FormatYdbNodeHostPort("2001:db8::1", 2136), "[2001:db8::1]:2136");
}

TEST(YdbNodeEndpoint, KeepsAlreadyBracketedIpv6) {
    EXPECT_EQ(FormatYdbNodeHostPort("[2001:db8::1]", 2135), "[2001:db8::1]:2135");
}

TEST(YdbNodeEndpoint, RejectsEmptyOrZeroPort) {
    EXPECT_TRUE(FormatYdbNodeHostPort("", 2135).empty());
    EXPECT_TRUE(FormatYdbNodeHostPort("host", 0).empty());
}

TEST(YdbNodeEndpoint, DedupsByNodeId) {
    const auto hostPorts = UniqueYdbNodeHostPorts({
        {50002, "n2", 2135},
        {50000, "n0", 2135},
        {50002, "n2-alias", 2135},
    });
    ASSERT_EQ(hostPorts.size(), 2u);
    EXPECT_EQ(hostPorts[0], "n2:2135");
    EXPECT_EQ(hostPorts[1], "n0:2135");
}

TEST(YdbNodeEndpoint, DedupsZeroNodeIdByHostPort) {
    const auto hostPorts = UniqueYdbNodeHostPorts({
        {0, "n1", 2135},
        {0, "n1", 2135},
        {0, "n2", 2135},
    });
    ASSERT_EQ(hostPorts.size(), 2u);
    EXPECT_EQ(hostPorts[0], "n1:2135");
    EXPECT_EQ(hostPorts[1], "n2:2135");
}

TEST(YdbNodeEndpoint, SkipsEmptyAddresses) {
    const auto hostPorts = UniqueYdbNodeHostPorts({
        {1, "", 2135},
        {2, "n2", 0},
        {3, "n3", 2135},
    });
    ASSERT_EQ(hostPorts.size(), 1u);
    EXPECT_EQ(hostPorts[0], "n3:2135");
}

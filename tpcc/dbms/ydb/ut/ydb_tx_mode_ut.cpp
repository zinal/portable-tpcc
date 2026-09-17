#include <gtest/gtest.h>

#include <ydb_tx_mode.h>

using namespace NTpcc;

TEST(YdbTxMode, DefaultsToSnapshotRw) {
    EIsolationLevel isolation = EIsolationLevel::Serializable;
    ASSERT_TRUE(ParseYdbTxMode("", isolation));
    EXPECT_EQ(isolation, EIsolationLevel::RepeatableRead);
    EXPECT_STREQ(YdbTxModeName(isolation), YDB_TX_MODE_SNAPSHOT_RW);
    EXPECT_EQ(
        YdbTxSettingsForIsolation(isolation).GetMode(),
        NYdb::NQuery::TTxSettings::TS_SNAPSHOT_RW);
}

TEST(YdbTxMode, ParsesSnapshotAndSerializable) {
    EIsolationLevel isolation = EIsolationLevel::ReadCommitted;
    ASSERT_TRUE(ParseYdbTxMode("snapshot-rw", isolation));
    EXPECT_EQ(isolation, EIsolationLevel::RepeatableRead);
    EXPECT_EQ(
        YdbTxSettingsForIsolation(isolation).GetMode(),
        NYdb::NQuery::TTxSettings::TS_SNAPSHOT_RW);

    ASSERT_TRUE(ParseYdbTxMode("serializable-rw", isolation));
    EXPECT_EQ(isolation, EIsolationLevel::Serializable);
    EXPECT_STREQ(YdbTxModeName(isolation), YDB_TX_MODE_SERIALIZABLE_RW);
    EXPECT_EQ(
        YdbTxSettingsForIsolation(isolation).GetMode(),
        NYdb::NQuery::TTxSettings::TS_SERIALIZABLE_RW);
}

TEST(YdbTxMode, RejectsUnknownValues) {
    EIsolationLevel isolation = EIsolationLevel::RepeatableRead;
    EXPECT_FALSE(ParseYdbTxMode("serializable", isolation));
    EXPECT_FALSE(ParseYdbTxMode("snapshot", isolation));
    EXPECT_FALSE(ParseYdbTxMode("read-committed-rw", isolation));
}

TEST(YdbTxMode, MapsReadCommittedToSnapshot) {
    EXPECT_STREQ(YdbTxModeName(EIsolationLevel::ReadCommitted), YDB_TX_MODE_SNAPSHOT_RW);
    EXPECT_EQ(
        YdbTxSettingsForIsolation(EIsolationLevel::ReadCommitted).GetMode(),
        NYdb::NQuery::TTxSettings::TS_SNAPSHOT_RW);
}

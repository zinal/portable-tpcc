#include <gtest/gtest.h>

#include <init.h>
#include <partition_config.h>

#include <stdexcept>

using namespace NTpcc;

TEST(PgPartitionConfig, DeriveHashCount) {
    EXPECT_EQ(DerivePgHashPartitionCount(1), 16);
    EXPECT_EQ(DerivePgHashPartitionCount(100), 16);
    EXPECT_EQ(DerivePgHashPartitionCount(1000), 23);
    EXPECT_EQ(DerivePgHashPartitionCount(10000), 223);
    EXPECT_THROW(DerivePgHashPartitionCount(0), std::runtime_error);
}

TEST(PgPartitionConfig, ResolveNoneAndHash) {
    TPgPartitionConfig none;
    none.Partitioning = PG_PARTITIONING_NONE;
    EXPECT_EQ(ResolvePgPartitionCount(none), 0);

    TPgPartitionConfig hashDerived;
    hashDerived.Partitioning = PG_PARTITIONING_WAREHOUSE_HASH;
    hashDerived.WarehouseCount = 1000;
    EXPECT_EQ(ResolvePgPartitionCount(hashDerived), 23);

    TPgPartitionConfig hashExplicit;
    hashExplicit.Partitioning = PG_PARTITIONING_WAREHOUSE_HASH;
    hashExplicit.PartitionCount = 32;
    EXPECT_EQ(ResolvePgPartitionCount(hashExplicit), 32);

    TPgPartitionConfig invalid;
    invalid.Partitioning = "range";
    EXPECT_THROW(ResolvePgPartitionCount(invalid), std::runtime_error);

    TPgPartitionConfig noneWithCount;
    noneWithCount.Partitioning = PG_PARTITIONING_NONE;
    noneWithCount.PartitionCount = 8;
    EXPECT_THROW(ResolvePgPartitionCount(noneWithCount), std::runtime_error);
}

TEST(PgPartitionConfig, SchemaDdlHashAndPlain) {
    const std::string plain = BuildTpccSchemaDdl(0);
    EXPECT_EQ(plain.find("PARTITION BY"), std::string::npos);
    EXPECT_NE(plain.find("CREATE TABLE stock"), std::string::npos);

    const std::string hashed = BuildTpccSchemaDdl(4);
    EXPECT_NE(hashed.find("PARTITION BY HASH (s_w_id)"), std::string::npos);
    EXPECT_NE(hashed.find("PARTITION BY HASH (c_w_id)"), std::string::npos);
    EXPECT_NE(hashed.find("PARTITION BY HASH (h_w_id)"), std::string::npos);
    EXPECT_NE(hashed.find("PARTITION BY HASH (o_w_id)"), std::string::npos);
    EXPECT_NE(hashed.find("PARTITION BY HASH (no_w_id)"), std::string::npos);
    EXPECT_NE(hashed.find("PARTITION BY HASH (ol_w_id)"), std::string::npos);
    EXPECT_EQ(hashed.find("PARTITION BY HASH (w_id)"), std::string::npos);
    EXPECT_EQ(hashed.find("PARTITION BY HASH (i_id)"), std::string::npos);
    EXPECT_NE(hashed.find("FOR VALUES WITH (MODULUS 4, REMAINDER 0)"), std::string::npos);
    EXPECT_NE(hashed.find("FOR VALUES WITH (MODULUS 4, REMAINDER 3)"), std::string::npos);
    EXPECT_EQ(hashed.find("FOR VALUES WITH (MODULUS 4, REMAINDER 4)"), std::string::npos);
}

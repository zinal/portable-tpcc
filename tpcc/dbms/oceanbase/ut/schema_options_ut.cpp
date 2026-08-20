#include <gtest/gtest.h>

#include <schema_options.h>

#include <stdexcept>

using namespace NTpcc;

TEST(ObSchemaOptions, ResolvePartitionCount) {
    TObSchemaOptions none;
    none.PartitionCount = -1;
    EXPECT_EQ(ResolveObPartitionCount(none), -1);
    EXPECT_EQ(ObPartitioningStyle(none), OB_PARTITIONING_NONE);

    TObSchemaOptions derived;
    derived.PartitionCount = 0;
    derived.WarehouseCount = 10;
    EXPECT_EQ(ResolveObPartitionCount(derived), 10);
    EXPECT_EQ(ObPartitioningStyle(derived), OB_PARTITIONING_TABLEGROUP_HASH);

    TObSchemaOptions explicitCount;
    explicitCount.PartitionCount = 64;
    explicitCount.WarehouseCount = 10;
    EXPECT_EQ(ResolveObPartitionCount(explicitCount), 64);

    TObSchemaOptions invalid;
    invalid.PartitionCount = -2;
    EXPECT_THROW(ResolveObPartitionCount(invalid), std::runtime_error);
}

TEST(ObSchemaOptions, ResolveAnalyzeDegreeMatchesPartitions) {
    TObSchemaOptions none;
    none.PartitionCount = -1;
    EXPECT_EQ(ResolveObAnalyzeDegree(none), 1);

    TObSchemaOptions derived;
    derived.PartitionCount = 0;
    derived.WarehouseCount = 10;
    EXPECT_EQ(ResolveObAnalyzeDegree(derived), 10);

    TObSchemaOptions explicitCount;
    explicitCount.PartitionCount = 64;
    explicitCount.WarehouseCount = 100;
    EXPECT_EQ(ResolveObAnalyzeDegree(explicitCount), 64);

    TObSchemaOptions oneWh;
    oneWh.PartitionCount = 0;
    oneWh.WarehouseCount = 1;
    EXPECT_EQ(ResolveObAnalyzeDegree(oneWh), 1);
}

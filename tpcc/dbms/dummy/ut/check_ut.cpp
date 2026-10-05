#include <gtest/gtest.h>

#include <catalog.h>
#include <check.h>

using namespace NTpcc;

TEST(DummyCheck, AfterImportPassesEveryCatalogId) {
    TCheckRequest req;
    req.WarehouseCount = 10;
    req.Phase = ECheckPhase::AfterImport;
    req.RunId = "run-1";
    req.Instance = "checker";
    const auto report = RunDummyChecks(req);
    EXPECT_TRUE(report.Ok());
    EXPECT_EQ(report.FailedCount, 0);
    EXPECT_EQ(report.ErrorCount, 0);
    EXPECT_EQ(report.PassedCount, CountCatalogChecks(ECheckPhase::AfterImport));
    EXPECT_EQ(static_cast<int>(report.Results.size()), report.PassedCount);
    EXPECT_EQ(report.Phase, "after-import");
}

TEST(DummyCheck, AfterTestOmitsImportOnlyEntries) {
    TCheckRequest req;
    req.WarehouseCount = 2;
    req.Phase = ECheckPhase::AfterTest;
    const auto report = RunDummyChecks(req);
    EXPECT_TRUE(report.Ok());
    EXPECT_EQ(report.PassedCount, CountCatalogChecks(ECheckPhase::AfterTest));
    EXPECT_LT(report.PassedCount, CountCatalogChecks(ECheckPhase::AfterImport));
    EXPECT_EQ(report.Phase, "after-test");
}

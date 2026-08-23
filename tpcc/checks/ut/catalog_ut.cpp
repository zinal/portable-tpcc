#include <catalog.h>
#include <report.h>
#include <types.h>

#include <library/cpp/testing/gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>

using namespace NTpcc;

TEST(CheckCatalog, ContainsCoreConsistencyIds) {
    const auto& catalog = CheckCatalog();
    ASSERT_FALSE(catalog.empty());
    EXPECT_NE(FindCheckCatalogEntry("consistency.3.3.2.1"), nullptr);
    EXPECT_NE(FindCheckCatalogEntry("consistency.3.3.2.12"), nullptr);
    EXPECT_NE(FindCheckCatalogEntry("post_import.w_ytd"), nullptr);
    EXPECT_NE(FindCheckCatalogEntry("post_import.ol_delivery_eq_entry"), nullptr);
    EXPECT_NE(FindCheckCatalogEntry("post_import.o_carrier_id_range"), nullptr);
    EXPECT_NE(FindCheckCatalogEntry("post_import.ol_amount_delivered"), nullptr);
    EXPECT_EQ(FindCheckCatalogEntry("does.not.exist"), nullptr);
}

TEST(CheckCatalog, PhaseFiltering) {
    EXPECT_TRUE(CheckAppliesToPhase(ECheckPhase::Both, ECheckPhase::AfterImport));
    EXPECT_TRUE(CheckAppliesToPhase(ECheckPhase::Both, ECheckPhase::AfterTest));
    EXPECT_TRUE(CheckAppliesToPhase(ECheckPhase::AfterImport, ECheckPhase::AfterImport));
    EXPECT_FALSE(CheckAppliesToPhase(ECheckPhase::AfterImport, ECheckPhase::AfterTest));
    EXPECT_TRUE(CheckAppliesToPhase(ECheckPhase::AfterTest, ECheckPhase::AfterTest));
}

TEST(CheckStatus, ToString) {
    EXPECT_STREQ(CheckStatusToString(ECheckStatus::Passed), "passed");
    EXPECT_STREQ(CheckStatusToString(ECheckStatus::Failed), "failed");
}

TEST(CheckReport, RecordCheckResultPrintsProgressLine) {
    TCheckReport report;
    std::ostringstream captured;
    auto* old = std::cout.rdbuf(captured.rdbuf());
    RecordCheckResult(report, "cardinality.warehouse", ECheckStatus::Passed, {}, true);
    RecordCheckResult(
        report, "cardinality.stock", ECheckStatus::Failed, "query returned false", true);
    RecordCheckResult(
        report, "consistency.3.3.2.1", ECheckStatus::Skipped, "skipped: base cardinality failed", true);
    std::cout.rdbuf(old);

    EXPECT_EQ(captured.str(),
              "Checking Warehouse cardinality [OK]\n"
              "Checking Stock cardinality [Failed]: query returned false\n"
              "Checking W_YTD equals sum(D_YTD) [Skipped]: skipped: base cardinality failed\n");
    EXPECT_EQ(report.PassedCount, 1);
    EXPECT_EQ(report.FailedCount, 1);
    EXPECT_EQ(report.SkippedCount, 1);
    ASSERT_EQ(report.Results.size(), 3u);
    EXPECT_EQ(report.Results[0].Title, "Warehouse cardinality");
    EXPECT_FALSE(report.Ok());
}

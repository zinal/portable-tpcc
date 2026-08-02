#include <catalog.h>
#include <types.h>

#include <library/cpp/testing/gtest/gtest.h>

using namespace NTpcc;

TEST(CheckCatalog, ContainsCoreConsistencyIds) {
    const auto& catalog = CheckCatalog();
    ASSERT_FALSE(catalog.empty());
    EXPECT_NE(FindCheckCatalogEntry("consistency.3.3.2.1"), nullptr);
    EXPECT_NE(FindCheckCatalogEntry("consistency.3.3.2.12"), nullptr);
    EXPECT_NE(FindCheckCatalogEntry("post_import.w_ytd"), nullptr);
    EXPECT_NE(FindCheckCatalogEntry("post_import.ol_delivery_eq_entry"), nullptr);
    EXPECT_EQ(FindCheckCatalogEntry("does.not.exist"), nullptr);
}

TEST(CheckCatalog, PhaseFiltering) {
    EXPECT_TRUE(CheckAppliesToPhase(ECheckPhase::Both, ECheckPhase::AfterImport));
    EXPECT_TRUE(CheckAppliesToPhase(ECheckPhase::Both, ECheckPhase::AfterRun));
    EXPECT_TRUE(CheckAppliesToPhase(ECheckPhase::AfterImport, ECheckPhase::AfterImport));
    EXPECT_FALSE(CheckAppliesToPhase(ECheckPhase::AfterImport, ECheckPhase::AfterRun));
    EXPECT_TRUE(CheckAppliesToPhase(ECheckPhase::AfterRun, ECheckPhase::AfterRun));
}

TEST(CheckStatus, ToString) {
    EXPECT_STREQ(CheckStatusToString(ECheckStatus::Passed), "passed");
    EXPECT_STREQ(CheckStatusToString(ECheckStatus::Failed), "failed");
}

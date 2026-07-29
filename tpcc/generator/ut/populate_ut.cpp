#include <gtest/gtest.h>

#include <money.h>
#include <rng.h>

#include <populate.h>
#include <seed.h>

#include <algorithm>
#include <vector>

using namespace NTpcc;
using namespace NTpcc::NGenerator;

TEST(Money, ParseAndFormat) {
    auto m = TMoney::FromMajorMinor(12, 34);
    EXPECT_EQ(m.ToString(), "12.34");
    EXPECT_EQ(TMoney::Parse("12.34"), m);
    EXPECT_EQ(TMoney::Parse("-10.00"), CUSTOMER_INITIAL_BALANCE);
    EXPECT_EQ(DISTRICT_INITIAL_YTD.ToString(), "30000.00");
    EXPECT_EQ(WAREHOUSE_INITIAL_YTD.ToString(), "300000.00");
}

TEST(Money, Arithmetic) {
    auto a = TMoney::FromCents(1050);
    auto b = TMoney::FromCents(25);
    EXPECT_EQ((a + b).Cents(), 1075);
    EXPECT_EQ((a - b).Cents(), 1025);
}

TEST(SeededRng, Deterministic) {
    TSeededRng a(42);
    TSeededRng b(42);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.Next(), b.Next());
    }
    TSeededRng c(43);
    EXPECT_NE(TSeededRng(42).Next(), c.Next());
}

TEST(SeededRng, ForkIndependent) {
    auto root = RootRng(12345);
    auto w1 = root.Fork(SaltWarehouse).Fork(1);
    auto w2 = root.Fork(SaltWarehouse).Fork(2);
    EXPECT_NE(w1.Next(), w2.Next());
}

TEST(Generator, SameSeedSameItem) {
    auto a = GenerateItem(731910246, 1);
    auto b = GenerateItem(731910246, 1);
    EXPECT_EQ(a.Id, b.Id);
    EXPECT_EQ(a.Name, b.Name);
    EXPECT_EQ(a.Price, b.Price);
    EXPECT_EQ(a.Data, b.Data);
    EXPECT_EQ(a.ImageId, b.ImageId);

    auto c = GenerateItem(731910246, 2);
    EXPECT_NE(a.Name, c.Name);
}

TEST(Generator, WarehouseYtd) {
    auto wh = GenerateWarehouse(1, 7);
    EXPECT_EQ(wh.Ytd, WAREHOUSE_INITIAL_YTD);
    auto d = GenerateDistrict(1, 7, 3);
    EXPECT_EQ(d.Ytd, DISTRICT_INITIAL_YTD);
    EXPECT_EQ(d.NextOrderId, CUSTOMERS_PER_DISTRICT + 1);
}

TEST(Generator, CustomerPermutationSize) {
    auto perm = InitialOrderCustomerPermutation(99, 1, 1);
    ASSERT_EQ(perm.size(), static_cast<size_t>(CUSTOMERS_PER_DISTRICT));
    std::vector<int> sorted = perm;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < CUSTOMERS_PER_DISTRICT; ++i) {
        EXPECT_EQ(sorted[i], i + 1);
    }
    auto perm2 = InitialOrderCustomerPermutation(99, 1, 1);
    EXPECT_EQ(perm, perm2);
}

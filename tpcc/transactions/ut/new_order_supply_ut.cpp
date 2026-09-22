#include <gtest/gtest.h>

#include <new_order_supply.h>
#include <rng.h>

#include <vector>

using namespace NTpcc;

namespace {

std::vector<int> ChooseAll(
    NTpcc::NDetail::TFastRng& rng,
    int home,
    size_t warehouseCount,
    int percent,
    int maxRemote,
    int numItems)
{
    std::vector<int> out;
    out.reserve(static_cast<size_t>(numItems));
    int chosen = 0;
    for (int i = 0; i < numItems; ++i) {
        const int supplier = ChooseNewOrderSupplyWarehouse(
            rng, home, warehouseCount, percent, maxRemote, chosen);
        out.push_back(supplier);
        if (supplier != home) {
            ++chosen;
        }
    }
    return out;
}

std::vector<int> LegacyUncapped(
    NTpcc::NDetail::TFastRng& rng,
    int home,
    size_t warehouseCount,
    int percent,
    int numItems)
{
    std::vector<int> out;
    out.reserve(static_cast<size_t>(numItems));
    for (int i = 0; i < numItems; ++i) {
        if (warehouseCount == 1 ||
            static_cast<int>(RandomNumber(rng, 1, 100)) > percent)
        {
            out.push_back(home);
        } else {
            int supplierID;
            do {
                supplierID = static_cast<int>(RandomNumber(rng, 1, warehouseCount));
            } while (supplierID == home);
            out.push_back(supplierID);
        }
    }
    return out;
}

int RemoteCount(const std::vector<int>& suppliers, int home) {
    int n = 0;
    for (int id : suppliers) {
        if (id != home) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST(NewOrderSupply, ZeroCapMatchesUncappedSelection) {
    constexpr int kHome = 3;
    constexpr size_t kWarehouses = 10;
    constexpr int kPercent = 40;
    constexpr int kItems = 15;
    for (uint64_t seed = 1; seed <= 32; ++seed) {
        TSeededRng legacyRng(seed);
        TSeededRng cappedRng(seed);
        const auto legacy = LegacyUncapped(legacyRng.Impl(), kHome, kWarehouses, kPercent, kItems);
        const auto capped = ChooseAll(cappedRng.Impl(), kHome, kWarehouses, kPercent, 0, kItems);
        EXPECT_EQ(capped, legacy) << "seed=" << seed;
    }
}

TEST(NewOrderSupply, CapForcesRemainingLinesHome) {
    TSeededRng rng(0x4E4F5244ULL);
    const auto suppliers = ChooseAll(rng.Impl(), /*home*/ 1, /*warehouses*/ 8, /*percent*/ 100, /*max*/ 2, 6);
    ASSERT_EQ(suppliers.size(), 6u);
    EXPECT_NE(suppliers[0], 1);
    EXPECT_NE(suppliers[1], 1);
    EXPECT_EQ(RemoteCount(suppliers, 1), 2);
    for (size_t i = 2; i < suppliers.size(); ++i) {
        EXPECT_EQ(suppliers[i], 1) << "index=" << i;
    }
}

TEST(NewOrderSupply, ZeroPercentStaysHomeEvenWithCap) {
    TSeededRng rng(7);
    const auto suppliers = ChooseAll(rng.Impl(), 4, 12, /*percent*/ 0, /*max*/ 3, 10);
    EXPECT_EQ(RemoteCount(suppliers, 4), 0);
}

TEST(NewOrderSupply, SingleWarehouseIsAlwaysHome) {
    TSeededRng rng(9);
    const auto suppliers = ChooseAll(rng.Impl(), 1, 1, /*percent*/ 100, /*max*/ 0, 8);
    EXPECT_EQ(RemoteCount(suppliers, 1), 0);
}

TEST(NewOrderSupply, UnlimitedPercentUsesOnlyRemoteWarehouses) {
    TSeededRng rng(11);
    const auto suppliers = ChooseAll(rng.Impl(), 2, 5, /*percent*/ 100, /*max*/ 0, 7);
    EXPECT_EQ(RemoteCount(suppliers, 2), 7);
}

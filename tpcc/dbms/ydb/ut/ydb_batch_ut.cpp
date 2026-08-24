#include <gtest/gtest.h>

#include <ydb_batch.h>

#include <ops.h>

using namespace NTpcc;

namespace {

TSemanticOp StockOp(
    int warehouseId,
    int itemId,
    int newQuantity,
    int orderedQuantity,
    int remoteIncrement,
    TMoney newYtd,
    int newOrderCount,
    int newRemoteCount)
{
    return TUpdateStock{
        .WarehouseID = warehouseId,
        .ItemID = itemId,
        .NewQuantity = newQuantity,
        .OrderedQuantity = orderedQuantity,
        .RemoteIncrement = remoteIncrement,
        .NewYtd = newYtd,
        .NewOrderCount = newOrderCount,
        .NewRemoteCount = newRemoteCount};
}

} // namespace

TEST(AllSemanticOpsAre, EmptyAndMixed) {
    EXPECT_FALSE(AllSemanticOpsAre<TUpdateStock>({}));

    std::vector<TSemanticOp> mixed{
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0),
        TInsertOrderLine{.WarehouseID = 1, .DistrictID = 1, .OrderID = 1, .LineNumber = 1}};
    EXPECT_FALSE(AllSemanticOpsAre<TUpdateStock>(mixed));
}

TEST(AllSemanticOpsAre, HomogeneousStock) {
    std::vector<TSemanticOp> ops{
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0),
        StockOp(1, 11, 40, 3, 1, TMoney::FromCents(300), 1, 1)};
    EXPECT_TRUE(AllSemanticOpsAre<TUpdateStock>(ops));
    EXPECT_FALSE(AllSemanticOpsAre<TInsertOrderLine>(ops));
}

TEST(AggregateYdbStockUpdates, UniqueKeysPreserveOrder) {
    std::vector<TSemanticOp> ops{
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0),
        StockOp(2, 11, 40, 3, 1, TMoney::FromCents(300), 1, 1)};
    const auto rows = AggregateYdbStockUpdates(ops);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].WarehouseID, 1);
    EXPECT_EQ(rows[0].ItemID, 10);
    EXPECT_EQ(rows[0].NewQuantity, 90);
    EXPECT_EQ(rows[0].NewYtd, TMoney::FromCents(500));
    EXPECT_EQ(rows[0].NewOrderCount, 1);
    EXPECT_EQ(rows[0].NewRemoteCount, 0);
    EXPECT_EQ(rows[1].WarehouseID, 2);
    EXPECT_EQ(rows[1].ItemID, 11);
    EXPECT_EQ(rows[1].NewQuantity, 40);
    EXPECT_EQ(rows[1].NewYtd, TMoney::FromCents(300));
    EXPECT_EQ(rows[1].NewOrderCount, 1);
    EXPECT_EQ(rows[1].NewRemoteCount, 1);
}

TEST(AggregateYdbStockUpdates, DuplicateItemKeepsLastAbsoluteCounters) {
    // Same (warehouse, item) on two New-Order lines: in-order mutation leaves
    // the last absolute quantity/ytd/order_cnt/remote_cnt as the UPSERT row.
    std::vector<TSemanticOp> ops{
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0),
        StockOp(1, 11, 50, 2, 0, TMoney::FromCents(200), 1, 0),
        StockOp(1, 10, 85, 3, 1, TMoney::FromCents(800), 2, 1)};
    const auto rows = AggregateYdbStockUpdates(ops);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].WarehouseID, 1);
    EXPECT_EQ(rows[0].ItemID, 10);
    EXPECT_EQ(rows[0].NewQuantity, 85);
    EXPECT_EQ(rows[0].NewYtd, TMoney::FromCents(800));
    EXPECT_EQ(rows[0].NewOrderCount, 2);
    EXPECT_EQ(rows[0].NewRemoteCount, 1);
    EXPECT_EQ(rows[1].ItemID, 11);
    EXPECT_EQ(rows[1].NewQuantity, 50);
    EXPECT_EQ(rows[1].NewOrderCount, 1);
}

TEST(AggregateYdbStockUpdates, IgnoresNonStockOps) {
    std::vector<TSemanticOp> ops{
        TInsertOrderLine{.WarehouseID = 1},
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0)};
    const auto rows = AggregateYdbStockUpdates(ops);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].ItemID, 10);
}

#include <gtest/gtest.h>

#include <ob_batch.h>
#include <ob_connection.h>
#include <ob_queries.h>

#include <ops.h>

#include <algorithm>
#include <stdexcept>
#include <string>

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

size_t CountToken(const std::string& sql, const std::string& token) {
    size_t n = 0;
    for (size_t pos = 0; (pos = sql.find(token, pos)) != std::string::npos; pos += token.size()) {
        ++n;
    }
    return n;
}

} // namespace

TEST(ObBatchAllSemanticOpsAre, EmptyAndMixed) {
    EXPECT_FALSE(AllSemanticOpsAre<TUpdateStock>({}));

    std::vector<TSemanticOp> mixed{
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0),
        TInsertOrderLine{.WarehouseID = 1, .DistrictID = 1, .OrderID = 1, .LineNumber = 1}};
    EXPECT_FALSE(AllSemanticOpsAre<TUpdateStock>(mixed));
}

TEST(ObBatchAllSemanticOpsAre, HomogeneousStock) {
    std::vector<TSemanticOp> ops{
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0),
        StockOp(1, 11, 40, 3, 1, TMoney::FromCents(300), 1, 1)};
    EXPECT_TRUE(AllSemanticOpsAre<TUpdateStock>(ops));
    EXPECT_FALSE(AllSemanticOpsAre<TInsertOrderLine>(ops));
}

TEST(UniqueItemIds, DedupsAndSorts) {
    const auto ids = UniqueItemIds({30, 10, 20, 10, 30});
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 20);
    EXPECT_EQ(ids[2], 30);
}

TEST(UniqueItemIds, SingleInvalidItem) {
    const auto ids = UniqueItemIds({INVALID_ITEM_ID});
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], INVALID_ITEM_ID);
}

TEST(UniqueSortedStockKeys, SortsByWarehouseThenItem) {
    const auto keys = UniqueSortedStockKeys({
        TStockKey{2, 5},
        TStockKey{1, 20},
        TStockKey{1, 7},
        TStockKey{2, 5},
        TStockKey{1, 7},
    });
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0].WarehouseID, 1);
    EXPECT_EQ(keys[0].ItemID, 7);
    EXPECT_EQ(keys[1].WarehouseID, 1);
    EXPECT_EQ(keys[1].ItemID, 20);
    EXPECT_EQ(keys[2].WarehouseID, 2);
    EXPECT_EQ(keys[2].ItemID, 5);
}

TEST(AggregateObStockUpdates, UniqueKeysPreserveOrder) {
    std::vector<TSemanticOp> ops{
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0),
        StockOp(2, 11, 40, 3, 1, TMoney::FromCents(300), 1, 1)};
    const auto rows = AggregateObStockUpdates(ops);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].WarehouseID, 1);
    EXPECT_EQ(rows[0].ItemID, 10);
    EXPECT_EQ(rows[0].NewQuantity, 90);
    EXPECT_EQ(rows[0].OrderedQuantity, 5);
    EXPECT_EQ(rows[0].RemoteIncrement, 0);
    EXPECT_EQ(rows[0].LineCount, 1);
    EXPECT_EQ(rows[1].WarehouseID, 2);
    EXPECT_EQ(rows[1].ItemID, 11);
    EXPECT_EQ(rows[1].NewQuantity, 40);
    EXPECT_EQ(rows[1].OrderedQuantity, 3);
    EXPECT_EQ(rows[1].RemoteIncrement, 1);
    EXPECT_EQ(rows[1].LineCount, 1);
}

TEST(AggregateObStockUpdates, DuplicateItemSumsIncrementsKeepsLastQuantity) {
    std::vector<TSemanticOp> ops{
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0),
        StockOp(1, 11, 50, 2, 0, TMoney::FromCents(200), 1, 0),
        StockOp(1, 10, 85, 3, 1, TMoney::FromCents(800), 2, 1)};
    const auto rows = AggregateObStockUpdates(ops);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].WarehouseID, 1);
    EXPECT_EQ(rows[0].ItemID, 10);
    EXPECT_EQ(rows[0].NewQuantity, 85);
    EXPECT_EQ(rows[0].OrderedQuantity, 8);
    EXPECT_EQ(rows[0].RemoteIncrement, 1);
    EXPECT_EQ(rows[0].LineCount, 2);
    EXPECT_EQ(rows[1].ItemID, 11);
    EXPECT_EQ(rows[1].NewQuantity, 50);
    EXPECT_EQ(rows[1].OrderedQuantity, 2);
    EXPECT_EQ(rows[1].LineCount, 1);
}

TEST(AggregateObStockUpdates, IgnoresNonStockOps) {
    std::vector<TSemanticOp> ops{
        TInsertOrderLine{.WarehouseID = 1},
        StockOp(1, 10, 90, 5, 0, TMoney::FromCents(500), 1, 0)};
    const auto rows = AggregateObStockUpdates(ops);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].ItemID, 10);
}

TEST(BuildObGetItemsSql, BoundedPlaceholders) {
    EXPECT_EQ(
        BuildObGetItemsSql(1),
        "SELECT i_id, i_price, i_name, i_data FROM item WHERE i_id IN (?)");
    const auto sql = BuildObGetItemsSql(15);
    EXPECT_EQ(std::count(sql.begin(), sql.end(), '?'), 15);
    EXPECT_EQ(sql, BuildObGetItemsSql(15));
    EXPECT_THROW(BuildObGetItemsSql(0), std::invalid_argument);
    EXPECT_THROW(BuildObGetItemsSql(16), std::invalid_argument);
}

TEST(BuildObGetStocksForUpdateSql, SortedLockQuery) {
    const auto sql = BuildObGetStocksForUpdateSql(2);
    EXPECT_EQ(
        sql,
        "SELECT s_w_id, s_i_id, s_quantity, s_ytd, s_order_cnt, s_remote_cnt, s_data, "
        "s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, "
        "s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 "
        "FROM stock WHERE (s_w_id, s_i_id) IN ((?,?),(?,?)) "
        "ORDER BY s_w_id, s_i_id FOR UPDATE");
    EXPECT_NE(sql.find("FOR UPDATE"), std::string::npos);
    EXPECT_EQ(std::count(sql.begin(), sql.end(), '?'), 4);
}

TEST(BuildObStockUpdateBatchSql, JoinUnionAll) {
    const auto one = BuildObStockUpdateBatchSql(1);
    EXPECT_EQ(std::count(one.begin(), one.end(), '?'), 6);
    EXPECT_NE(one.find("INNER JOIN"), std::string::npos);
    EXPECT_EQ(one.find("UNION ALL"), std::string::npos);

    const auto two = BuildObStockUpdateBatchSql(2);
    EXPECT_EQ(CountToken(two, "UNION ALL"), 1u);
    EXPECT_EQ(std::count(two.begin(), two.end(), '?'), 12);
    EXPECT_NE(two.find("s_ytd = s.s_ytd + u.ytd_inc"), std::string::npos);
    EXPECT_THROW(BuildObStockUpdateBatchSql(0), std::invalid_argument);
    EXPECT_THROW(BuildObStockUpdateBatchSql(16), std::invalid_argument);
}

TEST(BuildObOrderLineInsertSql, MultiValue) {
    EXPECT_EQ(
        BuildObOrderLineInsertSql(1),
        "INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, "
        "ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES "
        "(?,?,?,?,?,?,?,?,?)");
    const auto sql = BuildObOrderLineInsertSql(10);
    EXPECT_EQ(std::count(sql.begin(), sql.end(), '?'), 90);
    EXPECT_EQ(CountToken(sql, "),("), 9u);
}

TEST(BuildObPaymentLocationSql, JoinUpdateThenSelect) {
    const auto sql = BuildObPaymentLocationSql(7, 3, "12.34");
    EXPECT_NE(sql.find("UPDATE warehouse w INNER JOIN district d"), std::string::npos);
    EXPECT_NE(sql.find("w.w_ytd = w.w_ytd + 12.34"), std::string::npos);
    EXPECT_NE(sql.find("d.d_ytd = d.d_ytd + 12.34"), std::string::npos);
    EXPECT_NE(sql.find("WHERE w.w_id = 7"), std::string::npos);
    EXPECT_NE(sql.find("d.d_id = 3"), std::string::npos);
    EXPECT_NE(sql.find("; SELECT w.w_name"), std::string::npos);
    EXPECT_EQ(sql.find("SKIP LOCKED"), std::string::npos);
}

TEST(BuildObOldestNewOrdersSql, TenDistrictsNoSkipLocked) {
    const auto sql = BuildObOldestNewOrdersSql(9);
    EXPECT_EQ(CountToken(sql, "FOR UPDATE"), static_cast<size_t>(DISTRICT_COUNT));
    EXPECT_EQ(CountToken(sql, "LIMIT 1 FOR UPDATE"), static_cast<size_t>(DISTRICT_COUNT));
    EXPECT_EQ(sql.find("SKIP LOCKED"), std::string::npos);
    EXPECT_EQ(CountToken(sql, ";"), static_cast<size_t>(DISTRICT_COUNT - 1));
    EXPECT_NE(sql.find("no_w_id = 9 AND no_d_id = 1 "), std::string::npos);
    EXPECT_NE(sql.find("no_w_id = 9 AND no_d_id = 10 "), std::string::npos);
    size_t pos = 0;
    for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
        const auto token = "no_d_id = " + std::to_string(d) + " ";
        const auto found = sql.find(token, pos);
        ASSERT_NE(found, std::string::npos) << token;
        pos = found + token.size();
    }
}

TEST(BuildObDeliveryOrderInfoSql, InListBothTables) {
    const auto sql = BuildObDeliveryOrderInfoSql(4, {{1, 100}, {3, 300}});
    EXPECT_NE(sql.find("FROM oorder WHERE o_w_id = 4 AND (o_d_id, o_id) IN ((1,100),(3,300))"),
              std::string::npos);
    EXPECT_NE(
        sql.find("FROM order_line WHERE ol_w_id = 4 AND (ol_d_id, ol_o_id) IN ((1,100),(3,300))"),
        std::string::npos);
    EXPECT_EQ(sql.find("SKIP LOCKED"), std::string::npos);
    EXPECT_THROW(BuildObDeliveryOrderInfoSql(1, {}), std::invalid_argument);
    EXPECT_THROW(
        BuildObDeliveryOrderInfoSql(1, std::vector<TObDeliveryOrderKey>(11, {1, 1})),
        std::invalid_argument);
}

TEST(BuildObDeliveryCompleteSql, ThreeDmlNoSkipLocked) {
    const auto sql = BuildObDeliveryCompleteSql(2, 7, {{1, 10}, {2, 20}});
    EXPECT_NE(sql.find("DELETE FROM new_order WHERE no_w_id = 2 AND (no_d_id, no_o_id) IN ((1,10),(2,20))"),
              std::string::npos);
    EXPECT_NE(sql.find("UPDATE oorder SET o_carrier_id = 7 WHERE o_w_id = 2"), std::string::npos);
    EXPECT_NE(sql.find("UPDATE order_line SET ol_delivery_d = CURRENT_TIMESTAMP"), std::string::npos);
    EXPECT_EQ(sql.find("SKIP LOCKED"), std::string::npos);
    EXPECT_EQ(CountToken(sql, ";"), 2u);
}

TEST(BuildObDeliveryApplySql, JoinUnionAll) {
    const auto one = BuildObDeliveryApplySql(1);
    EXPECT_EQ(std::count(one.begin(), one.end(), '?'), 4);
    EXPECT_EQ(one.find("UNION ALL"), std::string::npos);
    EXPECT_NE(one.find("c.c_balance = c.c_balance + u.amount"), std::string::npos);
    EXPECT_NE(one.find("c.c_delivery_cnt = c.c_delivery_cnt + 1"), std::string::npos);

    const auto ten = BuildObDeliveryApplySql(10);
    EXPECT_EQ(CountToken(ten, "UNION ALL"), 9u);
    EXPECT_EQ(std::count(ten.begin(), ten.end(), '?'), 40);
    EXPECT_THROW(BuildObDeliveryApplySql(0), std::invalid_argument);
    EXPECT_THROW(BuildObDeliveryApplySql(11), std::invalid_argument);
}

TEST(BuildObDeliveryFinishSql, UpdateAndCommit) {
    const auto sql = BuildObDeliveryFinishSql(TApplyDeliveryToCustomer{
        .WarehouseID = 5,
        .DistrictID = 2,
        .CustomerID = 17,
        .Amount = TMoney::FromCents(12345)});
    EXPECT_EQ(
        sql,
        "UPDATE customer SET c_balance = c_balance + 123.45, c_delivery_cnt = c_delivery_cnt + 1 "
        "WHERE c_w_id = 5 AND c_d_id = 2 AND c_id = 17; COMMIT");
}

TEST(BuildObPaymentFinishSql, CustomerHistoryCommit) {
    TUpdateCustomerPayment update;
    update.WarehouseID = 1;
    update.DistrictID = 2;
    update.CustomerID = 3;
    update.NewBalance = TMoney::FromCents(-500);
    update.NewYtdPayment = TMoney::FromCents(500);
    update.NewPaymentCount = 4;
    update.UpdateData = false;

    TInsertPaymentHistory history;
    history.CustomerWarehouseID = 1;
    history.CustomerDistrictID = 2;
    history.CustomerID = 3;
    history.PaymentWarehouseID = 1;
    history.PaymentDistrictID = 2;
    history.Amount = TMoney::FromCents(500);
    history.Data = "WNAME    DNAME";

    const auto quotedHistory = QuoteSqlString(history.Data);
    const auto sql = BuildObPaymentFinishSql(update, history, "''", quotedHistory);
    EXPECT_NE(sql.find("c_balance = -5.00"), std::string::npos);
    EXPECT_NE(sql.find("c_ytd_payment = 5.00"), std::string::npos);
    EXPECT_NE(sql.find("c_payment_cnt = 4"), std::string::npos);
    EXPECT_EQ(sql.find("c_data ="), std::string::npos);
    EXPECT_NE(sql.find("INSERT INTO history"), std::string::npos);
    EXPECT_NE(sql.find(quotedHistory), std::string::npos);
    EXPECT_NE(sql.find("; COMMIT"), std::string::npos);

    update.UpdateData = true;
    update.NewData = "3 2 1 2 1 5.00 | old";
    const auto withData = BuildObPaymentFinishSql(
        update, history, QuoteSqlString(update.NewData), quotedHistory);
    EXPECT_NE(withData.find("c_data = '3 2 1 2 1 5.00 | old'"), std::string::npos);
}

TEST(ObPaymentCustomerForUpdateSql, LockingClause) {
    EXPECT_NE(std::string(QuerySql(EObQueryId::GetCustomerByIdForUpdate)).find("FOR UPDATE"),
              std::string::npos);
    EXPECT_EQ(std::string(QuerySql(EObQueryId::GetCustomerById)).find("FOR UPDATE"),
              std::string::npos);
    const auto byName = std::string(QuerySql(EObQueryId::GetCustomersByLastNameForUpdate));
    EXPECT_NE(byName.find("ORDER BY c_first FOR UPDATE"), std::string::npos);
    EXPECT_EQ(std::string(QuerySql(EObQueryId::GetCustomersByLastName)).find("FOR UPDATE"),
              std::string::npos);
}

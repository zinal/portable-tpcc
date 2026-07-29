#include <gtest/gtest.h>

#include <ops.h>
#include <session.h>

using namespace NTpcc;

TEST(SemanticOp, VariantHoldsOps) {
    TSemanticOp op = TGetWarehouseTax{.WarehouseID = 1};
    EXPECT_TRUE(std::holds_alternative<TGetWarehouseTax>(op));
    EXPECT_EQ(std::get<TGetWarehouseTax>(op).WarehouseID, 1);

    TOperationResult result;
    result.Ok = true;
    result.Payload = TMoney::FromMajorMinor(1, 50);
    EXPECT_TRUE(std::holds_alternative<TMoney>(result.Payload));
    EXPECT_EQ(std::get<TMoney>(result.Payload).ToString(), "1.50");
}

TEST(SessionApi, TypesLink) {
    // Ensure async result types are instantiable with TFuture.
    TPromise<TOperationResult> promise;
    TFuture<TOperationResult> future = promise.GetFuture();
    TOperationResult value;
    value.Ok = true;
    promise.SetValue(value);
    EXPECT_TRUE(future.Get().Ok);
}

#include <gtest/gtest.h>

#include <context.h>
#include <ops.h>

using namespace NTpcc;

namespace {

TCustomerRow MakeCustomer(int id, std::string first) {
    TCustomerRow c;
    c.CustomerID = id;
    c.First = std::move(first);
    return c;
}

} // namespace

TEST(SelectCustomerByLastNameMedian, Empty) {
    EXPECT_FALSE(SelectCustomerByLastNameMedian({}).has_value());
}

TEST(SelectCustomerByLastNameMedian, SingleAndEvenOdd) {
    {
        auto c = SelectCustomerByLastNameMedian({MakeCustomer(1, "A")});
        ASSERT_TRUE(c);
        EXPECT_EQ(c->CustomerID, 1);
    }
    {
        // n=2 → position 1 (1-based) → index 0
        auto c = SelectCustomerByLastNameMedian({MakeCustomer(1, "A"), MakeCustomer(2, "B")});
        ASSERT_TRUE(c);
        EXPECT_EQ(c->CustomerID, 1);
    }
    {
        // n=3 → position 2 → index 1
        auto c = SelectCustomerByLastNameMedian({
            MakeCustomer(1, "A"), MakeCustomer(2, "B"), MakeCustomer(3, "C")});
        ASSERT_TRUE(c);
        EXPECT_EQ(c->CustomerID, 2);
    }
}

TEST(ClassifiedError, CarriesClass) {
    TClassifiedError err(EErrorClass::RetryableAbort, "40001", "serialization");
    EXPECT_EQ(err.Class, EErrorClass::RetryableAbort);
    EXPECT_EQ(err.NativeCode, "40001");
}

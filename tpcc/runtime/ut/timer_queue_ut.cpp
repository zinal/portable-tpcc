#include <gtest/gtest.h>

#include <timer_queue.h>

#include <chrono>
#include <vector>

using namespace NTpcc;
using namespace std::chrono;

TEST(BinnedTimerQueue, AddAndPopInOrder) {
    TBinnedTimerQueue<int> queue(8, 100);
    const auto now = steady_clock::time_point{};

    queue.Add(milliseconds(30), 3, now);
    queue.Add(milliseconds(10), 1, now);
    queue.Add(milliseconds(20), 2, now);
    ASSERT_TRUE(queue.Validate());

    EXPECT_EQ(queue.PopFront().Value, 1);
    EXPECT_EQ(queue.PopFront().Value, 2);
    EXPECT_EQ(queue.PopFront().Value, 3);
    EXPECT_TRUE(queue.Empty());
    ASSERT_TRUE(queue.Validate());
}

TEST(BinnedTimerQueue, GetNextDeadlineUsesMinOfUnsortedNextBucket) {
    // soft limit 1 forces later deadlines into the next bin; AddBack leaves
    // that bin unsorted so Items[0] is not the earliest deadline.
    TBinnedTimerQueue<int> queue(4, 1);
    const auto now = steady_clock::time_point{};

    queue.Add(milliseconds(10), 1, now);
    queue.Add(milliseconds(100), 100, now);
    queue.Add(milliseconds(50), 50, now);
    ASSERT_TRUE(queue.Validate());

    EXPECT_EQ(queue.PopFront().Value, 1);
    ASSERT_TRUE(queue.Validate());

    // Regression: must not return Items[0] (deadline 100ms) from the unsorted
    // next bin while a 50ms timer is still pending there.
    EXPECT_EQ(queue.GetNextDeadline(), now + milliseconds(50));

    EXPECT_EQ(queue.PopFront().Value, 50);
    EXPECT_EQ(queue.GetNextDeadline(), now + milliseconds(100));
    EXPECT_EQ(queue.PopFront().Value, 100);
    EXPECT_TRUE(queue.Empty());
    EXPECT_EQ(queue.GetNextDeadline(), steady_clock::time_point::max());
}

TEST(BinnedTimerQueue, PopFrontSkipsEmptyBucketsAfterSoftLimitSplit) {
    TBinnedTimerQueue<int> queue(4, 1);
    const auto now = steady_clock::time_point{};

    queue.Add(milliseconds(10), 1, now);
    queue.Add(milliseconds(200), 200, now);
    queue.Add(milliseconds(100), 100, now);
    ASSERT_TRUE(queue.Validate());

    EXPECT_EQ(queue.PopFront().Value, 1);
    EXPECT_EQ(queue.PopFront().Value, 100);
    EXPECT_EQ(queue.PopFront().Value, 200);
    EXPECT_TRUE(queue.Empty());
    ASSERT_TRUE(queue.Validate());
}

TEST(BinnedTimerQueue, ProcessDueUsesEarliestUnsortedDeadline) {
    TBinnedTimerQueue<int> queue(4, 1);
    const auto now = steady_clock::time_point{};

    queue.Add(milliseconds(10), 1, now);
    queue.Add(milliseconds(100), 100, now);
    queue.Add(milliseconds(50), 50, now);
    EXPECT_EQ(queue.PopFront().Value, 1);

    // At t=60ms the 50ms timer is due; the 100ms timer is not.
    const auto t60 = now + milliseconds(60);
    std::vector<int> due;
    while (!queue.Empty() && queue.GetNextDeadline() <= t60) {
        due.push_back(queue.PopFront().Value);
    }
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0], 50);
    EXPECT_EQ(queue.Size(), 1u);
    EXPECT_EQ(queue.GetNextDeadline(), now + milliseconds(100));
}

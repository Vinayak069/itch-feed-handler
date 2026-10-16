#include <gtest/gtest.h>

#include <thread>

#include "feed/mutex_queue.hpp"
#include "feed/spsc_queue.hpp"

using feed::SpscQueue;

TEST(Spsc, FifoAndCapacity) {
    SpscQueue<int> q(4);
    EXPECT_THROW(SpscQueue<int>(6), std::invalid_argument);
    int v;
    EXPECT_FALSE(q.try_pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(4));  // full
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.try_pop(v));
}

TEST(Spsc, WrapsAroundManyTimes) {
    SpscQueue<uint64_t> q(8);
    uint64_t v;
    for (uint64_t i = 0; i < 1000; ++i) {
        ASSERT_TRUE(q.try_push(i));
        ASSERT_TRUE(q.try_push(i + 1));
        ASSERT_TRUE(q.try_pop(v));
        ASSERT_EQ(v, i);
        ASSERT_TRUE(q.try_pop(v));
        ASSERT_EQ(v, i + 1);
    }
}

TEST(Spsc, ReserveAndPublish) {
    struct Big { uint64_t a[32]; };
    SpscQueue<Big> q(2);
    Big* s = q.try_reserve();
    ASSERT_NE(s, nullptr);
    s->a[31] = 42;
    EXPECT_EQ(q.front(), nullptr);  // not visible before publish
    q.publish();
    ASSERT_NE(q.front(), nullptr);
    EXPECT_EQ(q.front()->a[31], 42u);
    q.pop();
}

// Two threads, tiny ring so it is constantly full or empty: every value must arrive once, in
// order. Run under -fsanitize=thread in CI to check the memory ordering.
template <class Q>
void stress(Q& q, uint64_t n) {
    std::thread prod([&] {
        for (uint64_t i = 1; i <= n; ++i)
            while (!q.try_push(i)) std::this_thread::yield();
    });
    uint64_t expect = 1, v;
    while (expect <= n) {
        if (q.try_pop(v)) {
            ASSERT_EQ(v, expect);
            ++expect;
        } else {
            std::this_thread::yield();
        }
    }
    prod.join();
}

TEST(Spsc, TwoThreadStressPadded) {
    SpscQueue<uint64_t, true> q(16);
    stress(q, 1'000'000);
}

TEST(Spsc, TwoThreadStressUnpadded) {
    SpscQueue<uint64_t, false> q(16);
    stress(q, 1'000'000);
}

TEST(Spsc, MutexBaselineStress) {
    feed::MutexQueue<uint64_t> q(16);
    stress(q, 200'000);
}

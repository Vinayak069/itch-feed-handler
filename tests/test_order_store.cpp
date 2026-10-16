#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

#include "book/order_store.hpp"

using book::Order;
using book::OrderStore;

TEST(OrderStore, Basics) {
    OrderStore s(16);
    EXPECT_TRUE(s.insert({1, 100, 10, 3, true}));
    EXPECT_FALSE(s.insert({1, 200, 20, 3, true}));  // duplicate reference
    ASSERT_NE(s.find(1), nullptr);
    EXPECT_EQ(s.find(1)->price, 100u);
    EXPECT_EQ(s.find(2), nullptr);
    EXPECT_TRUE(s.erase(1));
    EXPECT_FALSE(s.erase(1));
    EXPECT_EQ(s.size(), 0u);
}

TEST(OrderStore, GrowsAndKeepsEverything) {
    OrderStore s(16);
    for (uint64_t r = 1; r <= 100'000; ++r) ASSERT_TRUE(s.insert({r, uint32_t(r), 1, 0, false}));
    EXPECT_GE(s.capacity(), 200'000u);
    for (uint64_t r = 1; r <= 100'000; ++r) {
        ASSERT_NE(s.find(r), nullptr);
        EXPECT_EQ(s.find(r)->price, uint32_t(r));
    }
}

// Random inserts and erases against std::unordered_map. Backward-shift deletion is the part
// most likely to be wrong (an entry moved past its home slot becomes unreachable), so the
// table is kept small to force long, wrapping probe runs.
TEST(OrderStore, MatchesUnorderedMapUnderChurn) {
    std::mt19937_64 rng(7);
    OrderStore s(64);
    std::unordered_map<uint64_t, uint32_t> ref;
    std::vector<uint64_t> keys;
    for (int i = 0; i < 400'000; ++i) {
        const int op = int(rng() % 10);
        if (op < 5 || keys.empty()) {
            const uint64_t k = rng() % 5000 + 1;  // small key space: plenty of duplicates
            const bool fresh = ref.emplace(k, uint32_t(i)).second;
            ASSERT_EQ(s.insert({k, uint32_t(i), 1, 0, true}), fresh);
            if (fresh) keys.push_back(k);
        } else {
            const std::size_t j = rng() % keys.size();
            const uint64_t k = keys[j];
            keys[j] = keys.back();
            keys.pop_back();
            ASSERT_TRUE(s.erase(k));
            ref.erase(k);
        }
        if (i % 997 == 0) {
            ASSERT_EQ(s.size(), ref.size());
            for (const auto& [k, v] : ref) {
                const Order* o = s.find(k);
                ASSERT_NE(o, nullptr) << "lost key " << k;
                ASSERT_EQ(o->price, v);
            }
        }
    }
}

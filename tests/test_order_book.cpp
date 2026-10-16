#include <gtest/gtest.h>

#include <memory>

#include "book/order_book.hpp"
#include "itch/framing.hpp"
#include "itch/generator.hpp"
#include "reference_book.hpp"

using book::OrderBook;

TEST(OrderBook, AddExecuteCancelDelete) {
    auto b = std::make_unique<OrderBook>(1024);
    EXPECT_TRUE(b->add(1, 5, true, 100, 10'000));    // first bid sets the top
    EXPECT_TRUE(b->add(2, 5, true, 200, 10'100));    // better bid
    EXPECT_FALSE(b->add(3, 5, true, 300, 9'900));    // behind the touch
    EXPECT_TRUE(b->add(4, 5, false, 50, 10'200));
    ASSERT_TRUE(b->best_bid(5));
    EXPECT_EQ(b->best_bid(5)->price, 10'100u);
    EXPECT_EQ(b->best_bid(5)->shares, 200u);
    EXPECT_EQ(b->best_ask(5)->price, 10'200u);

    EXPECT_TRUE(b->execute(2, 150));                 // partial fill at the touch
    EXPECT_EQ(b->best_bid(5)->shares, 50u);
    EXPECT_TRUE(b->execute(2, 50));                  // rest of it: level disappears
    EXPECT_EQ(b->best_bid(5)->price, 10'000u);
    EXPECT_FALSE(b->remove(3));                      // deep order: top unchanged
    EXPECT_EQ(b->stock(5)->bids.levels().size(), 1u);
    EXPECT_EQ(b->live_orders(), 2u);

    EXPECT_FALSE(b->remove(99));
    EXPECT_EQ(b->stats().unknown_ref, 1u);
    EXPECT_FALSE(b->add(1, 5, true, 1, 1));
    EXPECT_EQ(b->stats().duplicate_ref, 1u);
}

TEST(OrderBook, ReplaceKeepsSideAndStockButMovesPrice) {
    auto b = std::make_unique<OrderBook>(1024);
    b->add(10, 7, false, 100, 20'000);
    b->add(11, 7, false, 100, 20'100);
    EXPECT_TRUE(b->replace(10, 12, 300, 20'100));    // best ask moves away
    EXPECT_EQ(b->best_ask(7)->price, 20'100u);
    EXPECT_EQ(b->best_ask(7)->shares, 400u);
    EXPECT_EQ(b->stock(7)->asks.levels().back().orders, 2u);
    EXPECT_FALSE(b->execute(10, 1));                 // old reference is gone
    EXPECT_FALSE(b->best_bid(7));
}

TEST(OrderBook, SameLevelOrdersAggregate) {
    auto b = std::make_unique<OrderBook>(1024);
    for (uint64_t r = 1; r <= 5; ++r) b->add(r, 1, true, 100, 50'000);
    EXPECT_EQ(b->best_bid(1)->shares, 500u);
    EXPECT_EQ(b->stock(1)->bids.levels().back().orders, 5u);
    b->remove(3);
    EXPECT_EQ(b->best_bid(1)->shares, 400u);
    EXPECT_EQ(b->stock(1)->bids.levels().back().orders, 4u);
}

// Replays a generated feed through both the real book and the std::map oracle and compares
// the top of book after every message, and every price level at the end.
TEST(OrderBook, MatchesReferenceOnGeneratedFeed) {
    itch::GenConfig cfg;
    cfg.messages = 300'000;
    cfg.symbols = 40;
    cfg.live_target = 20'000;
    cfg.seed = 3;
    itch::GenStats gs;
    const auto framed = itch::generate_framed(cfg, &gs);
    EXPECT_GT(gs.by_type['A'], 0u);
    EXPECT_GT(gs.by_type['U'], 0u);
    EXPECT_GT(gs.by_type['X'], 0u);
    EXPECT_GT(gs.by_type['C'], 0u);

    auto b = std::make_unique<OrderBook>(1 << 15);
    RefBook ref;
    uint64_t checked = 0;
    const auto r = itch::for_each_framed(framed, [&](const std::byte* m, std::size_t) {
        b->apply(m);
        ref.apply(m);
        const char t = char(m[0]);
        if (t == 'S' || t == 'R' || t == 'P') return;
        const uint16_t loc = itch::decode_header(m).locate;
        for (bool buy : {true, false}) {
            const auto mine = buy ? b->best_bid(loc) : b->best_ask(loc);
            const auto theirs = ref.best(loc, buy);
            ASSERT_EQ(bool(mine), bool(theirs));
            if (mine) {
                ASSERT_EQ(mine->price, theirs->first);
                ASSERT_EQ(mine->shares, theirs->second);
            }
        }
        const auto bid = b->best_bid(loc), ask = b->best_ask(loc);
        if (bid && ask) {
            ASSERT_LT(bid->price, ask->price) << "generator produced a crossed book";
        }
        ++checked;
    });
    ASSERT_EQ(r.error, itch::FrameError::None);
    EXPECT_GT(checked, 290'000u);
    EXPECT_EQ(b->stats().unknown_ref, 0u);
    EXPECT_EQ(b->stats().duplicate_ref, 0u);
    EXPECT_EQ(b->live_orders(), ref.orders.size());

    for (uint16_t loc = 1; loc <= cfg.symbols; ++loc) {
        const auto* s = b->stock(loc);
        ASSERT_NE(s, nullptr);
        for (bool buy : {true, false}) {
            const auto& mine = (buy ? s->bids : s->asks).levels();
            auto& theirs = (buy ? ref.bids : ref.asks)[loc];
            ASSERT_EQ(mine.size(), theirs.size());
            std::size_t i = 0;
            // mine is worst -> best; std::map is ascending price
            if (buy) {
                for (const auto& [px, lv] : theirs) {
                    EXPECT_EQ(mine[i].price, px);
                    EXPECT_EQ(mine[i].shares, lv.shares);
                    EXPECT_EQ(mine[i].orders, lv.orders);
                    ++i;
                }
            } else {
                for (auto it = theirs.rbegin(); it != theirs.rend(); ++it, ++i) {
                    EXPECT_EQ(mine[i].price, it->first);
                    EXPECT_EQ(mine[i].shares, it->second.shares);
                }
            }
        }
    }
}

TEST(Generator, MixAndLiveOrdersFollowConfig) {
    itch::GenConfig cfg;
    cfg.messages = 1'000'000;
    cfg.symbols = 500;
    cfg.live_target = 100'000;
    itch::Generator g(cfg);
    std::byte msg[64];
    while (g.next(msg)) {}
    const auto& s = g.stats();
    uint64_t total = 0;
    for (auto c : s.by_type) total += c;
    EXPECT_EQ(total, cfg.messages);
    EXPECT_EQ(s.by_type['R'], cfg.symbols);
    EXPECT_NEAR(double(g.live_orders()), 100'000.0, 6'000.0);
    // After warm-up the shares of deletes and replaces track the configured weights.
    const double flow = double(cfg.messages - cfg.live_target);
    EXPECT_NEAR(double(s.by_type['U']) / flow, 80.5 / 984.3, 0.01);
}

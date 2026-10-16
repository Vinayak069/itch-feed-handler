// Runs the whole live path over real UDP multicast on the loopback interface.
#include <gtest/gtest.h>

#include <memory>

#include "feed/pipeline.hpp"
#include "itch/generator.hpp"
#include "reference_book.hpp"

namespace {

struct Feed {
    std::vector<std::byte> framed;
    itch::Packetized packets;
};

Feed make_feed(uint64_t messages) {
    itch::GenConfig cfg;
    cfg.messages = messages;
    cfg.symbols = 100;
    cfg.live_target = 20'000;
    Feed f;
    f.framed = itch::generate_framed(cfg);
    f.packets = itch::packetize(f.framed, "TEST000001");
    return f;
}

}  // namespace

TEST(Pipeline, DeliversEveryMessageAndBuildsTheSameBook) {
    const Feed f = make_feed(200'000);
    feed::PipelineConfig cfg;
    cfg.port = 30111;
    cfg.rate = 2e6;  // paced, so loopback does not drop packets
    cfg.idle_timeout_ns = 300'000'000;
    auto b = std::make_unique<book::OrderBook>(1 << 16);
    const auto r = feed::run_pipeline(f.packets, cfg, *b);

    EXPECT_EQ(r.packets_sent, f.packets.offset.size());
    EXPECT_EQ(r.packets_received, r.packets_sent);
    EXPECT_EQ(r.bad_packets, 0u);
    EXPECT_EQ(r.gaps, 0u);
    EXPECT_EQ(r.messages_applied, 200'000u);
    EXPECT_EQ(r.end_to_end.count(), r.packets_received);

    RefBook ref;
    itch::for_each_framed(f.framed, [&](const std::byte* m, std::size_t) { ref.apply(m); });
    EXPECT_EQ(b->live_orders(), ref.orders.size());
    EXPECT_EQ(b->stats().unknown_ref, 0u);
    for (uint16_t loc = 1; loc <= 100; ++loc) {
        const auto bid = b->best_bid(loc);
        const auto rb = ref.best(loc, true);
        ASSERT_EQ(bool(bid), bool(rb));
        if (bid) {
            EXPECT_EQ(bid->price, rb->first);
        }
    }
}

TEST(Pipeline, DetectsDroppedPackets) {
    const Feed f = make_feed(100'000);
    feed::PipelineConfig cfg;
    cfg.port = 30112;
    cfg.rate = 2e6;
    cfg.drop_every = 50;  // sender skips packets 50, 100, ...
    cfg.idle_timeout_ns = 300'000'000;
    auto b = std::make_unique<book::OrderBook>(1 << 16);
    const auto r = feed::run_pipeline(f.packets, cfg, *b);

    const uint64_t n = f.packets.offset.size();
    EXPECT_EQ(r.packets_skipped, n / 50);
    EXPECT_EQ(r.packets_received, n - n / 50);
    // Every skipped packet that is followed by another is a detected gap; a skipped final packet is not.
    const uint64_t trailing = (n % 50 == 0) ? 1 : 0;
    EXPECT_EQ(r.gaps, n / 50 - trailing);
    EXPECT_EQ(r.messages_applied + r.lost_messages + (trailing ? f.packets.messages - f.packets.first_seq.back() + 1 : 0),
              f.packets.messages);
    EXPECT_GT(b->stats().unknown_ref, 0u);  // lost adds make later deletes unknown: why real handlers recover
}

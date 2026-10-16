#include <gtest/gtest.h>

#include <vector>

#include "itch/framing.hpp"
#include "itch/generator.hpp"

using namespace itch;

namespace {

std::vector<std::byte> framed_deletes(int n) {
    std::vector<std::byte> out;
    std::byte msg[64];
    for (int i = 0; i < n; ++i) {
        const std::size_t len = encode(OrderDelete{{'D', 1, 0, uint64_t(i)}, uint64_t(i + 1)}, msg);
        const std::size_t at = out.size();
        out.resize(at + 2 + len);
        put16(out.data() + at, uint16_t(len));
        std::memcpy(out.data() + at + 2, msg, len);
    }
    return out;
}

}  // namespace

TEST(Framing, ReadsAllMessages) {
    const auto buf = framed_deletes(10);
    std::vector<uint64_t> refs;
    const auto r = for_each_framed(buf, [&](const std::byte* m, std::size_t len) {
        EXPECT_EQ(len, 19u);
        refs.push_back(decode_delete(m).ref);
    });
    EXPECT_EQ(r.error, FrameError::None);
    EXPECT_EQ(r.messages, 10u);
    EXPECT_EQ(r.consumed, buf.size());
    EXPECT_EQ(refs.front(), 1u);
    EXPECT_EQ(refs.back(), 10u);
}

TEST(Framing, TruncatedTailIsCarriedOver) {
    const auto buf = framed_deletes(3);
    for (std::size_t cut = 1; cut < 21; ++cut) {
        const std::span<const std::byte> part(buf.data(), buf.size() - cut);
        const auto r = for_each_framed(part, [](const std::byte*, std::size_t) {});
        EXPECT_EQ(r.error, FrameError::None);
        EXPECT_EQ(r.messages, 2u) << "cut " << cut;
        EXPECT_EQ(r.consumed, 42u);
    }
}

TEST(Framing, RejectsWrongLengthAndUnknownType) {
    auto buf = framed_deletes(2);
    put16(buf.data() + 21, 18);  // second message claims 18 bytes; a Delete is 19
    auto r = for_each_framed(buf, [](const std::byte*, std::size_t) {});
    EXPECT_EQ(r.error, FrameError::BadLength);
    EXPECT_EQ(r.messages, 1u);

    buf = framed_deletes(2);
    buf[2] = std::byte('z');
    r = for_each_framed(buf, [](const std::byte*, std::size_t) {});
    EXPECT_EQ(r.error, FrameError::UnknownType);
    EXPECT_EQ(r.messages, 0u);
}

TEST(Mold, PacketizeRoundTrip) {
    GenConfig cfg;
    cfg.messages = 20'000;
    cfg.symbols = 50;
    cfg.live_target = 2'000;
    const auto framed = generate_framed(cfg);
    const auto pk = packetize(framed, "SESSION001", 1000);
    EXPECT_EQ(pk.messages, cfg.messages);
    EXPECT_EQ(pk.first_seq.front(), 1000u);

    std::vector<std::byte> rebuilt;
    uint64_t expect_seq = 1000;
    for (std::size_t i = 0; i < pk.offset.size(); ++i) {
        ASSERT_LE(pk.length[i], kMaxPacket);
        const std::span<const std::byte> pkt(pk.bytes.data() + pk.offset[i], pk.length[i]);
        const MoldHeader h = decode_mold_header(pkt.data());
        EXPECT_EQ(std::string(h.session.data(), 10), "SESSION001");
        EXPECT_EQ(h.sequence, expect_seq);
        EXPECT_EQ(h.sequence, pk.first_seq[i]);
        const auto err = for_each_in_packet(pkt, [&](const std::byte* m, std::size_t len) {
            const std::size_t at = rebuilt.size();
            rebuilt.resize(at + 2 + len);
            put16(rebuilt.data() + at, uint16_t(len));
            std::memcpy(rebuilt.data() + at + 2, m, len);
        });
        EXPECT_EQ(err, PacketError::None);
        expect_seq += h.count;
    }
    EXPECT_EQ(rebuilt, framed);
}

TEST(Mold, CountMismatchAndShortPacket) {
    const auto pk = packetize(framed_deletes(5), "S");
    std::vector<std::byte> pkt(pk.bytes.begin(), pk.bytes.begin() + pk.length[0]);
    put16(pkt.data() + 18, 6);
    EXPECT_EQ(for_each_in_packet(pkt, [](const std::byte*, std::size_t) {}), PacketError::CountMismatch);
    EXPECT_EQ(for_each_in_packet(std::span(pkt).first(10), [](const std::byte*, std::size_t) {}), PacketError::TooShort);
    pkt.pop_back();  // a packet must hold whole messages
    put16(pkt.data() + 18, 5);
    EXPECT_EQ(for_each_in_packet(pkt, [](const std::byte*, std::size_t) {}), PacketError::BadMessage);
}

TEST(Mold, GapDetector) {
    GapDetector g;
    EXPECT_EQ(g.on_packet(1, 10), 0u);   // 1..10
    EXPECT_EQ(g.on_packet(11, 5), 0u);   // 11..15
    EXPECT_EQ(g.on_packet(21, 5), 5u);   // 16..20 lost
    EXPECT_EQ(g.on_packet(16, 5), 0u);   // late retransmission: not a new gap
    EXPECT_EQ(g.on_packet(26, 1), 0u);
    EXPECT_EQ(g.gaps(), 1u);
    EXPECT_EQ(g.lost_messages(), 5u);
    EXPECT_EQ(g.next_expected(), 27u);
}

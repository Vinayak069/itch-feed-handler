#include <gtest/gtest.h>

#include "itch/messages.hpp"

using namespace itch;

TEST(Messages, SpecSizes) {
    // Sizes from the ITCH 5.0 specification, section 1.
    EXPECT_EQ(kMessageSize['S'], 12);
    EXPECT_EQ(kMessageSize['R'], 39);
    EXPECT_EQ(kMessageSize['A'], 36);
    EXPECT_EQ(kMessageSize['F'], 40);
    EXPECT_EQ(kMessageSize['E'], 31);
    EXPECT_EQ(kMessageSize['C'], 36);
    EXPECT_EQ(kMessageSize['X'], 23);
    EXPECT_EQ(kMessageSize['D'], 19);
    EXPECT_EQ(kMessageSize['U'], 35);
    EXPECT_EQ(kMessageSize['P'], 44);
    EXPECT_EQ(kMessageSize['I'], 50);
    EXPECT_EQ(kMessageSize['Z'], 0);
}

TEST(Messages, AddOrderKnownBytes) {
    // Hand-assembled Add Order: locate 7, tracking 2, ts 0x010203040506, ref 0x1122334455667788,
    // buy 100 shares of "AAPL" at $150.25 (1502500 = 0x0016ED24).
    const unsigned char raw[36] = {'A', 0x00, 0x07, 0x00, 0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                   0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 'B', 0x00, 0x00, 0x00, 0x64,
                                   'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ', 0x00, 0x16, 0xED, 0x24};
    const auto* p = reinterpret_cast<const std::byte*>(raw);
    const AddOrder m = decode_add(p);
    EXPECT_EQ(m.h.type, 'A');
    EXPECT_EQ(m.h.locate, 7);
    EXPECT_EQ(m.h.tracking, 2);
    EXPECT_EQ(m.h.timestamp_ns, 0x010203040506ull);
    EXPECT_EQ(m.ref, 0x1122334455667788ull);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 100u);
    EXPECT_EQ(m.stock, stock_name("AAPL"));
    EXPECT_EQ(m.price, 1502500u);
    EXPECT_FALSE(m.mpid);

    std::byte out[64];
    ASSERT_EQ(encode(m, out), 36u);
    EXPECT_EQ(std::memcmp(out, raw, 36), 0);
}

TEST(Messages, RoundTripAllOrderTypes) {
    std::byte buf[64];
    const Header h{'?', 4242, 9, 34'200'000'000'123ull};
    auto with = [&](char t) { Header x = h; x.type = t; return x; };

    AddOrder f{with('F'), 99, 'S', 300, stock_name("MSFT"), 3'301'200, std::array<char, 4>{'G', 'S', 'C', 'O'}};
    ASSERT_EQ(encode(f, buf), 40u);
    const auto f2 = decode_add(buf);
    EXPECT_EQ(f2.ref, 99u);
    EXPECT_EQ(f2.side, 'S');
    ASSERT_TRUE(f2.mpid);
    EXPECT_EQ(std::string(f2.mpid->data(), 4), "GSCO");

    ASSERT_EQ(encode(OrderExecuted{with('E'), 5, 40, 777}, buf), 31u);
    const auto e = decode_executed(buf);
    EXPECT_EQ(e.h.locate, 4242);
    EXPECT_EQ(e.h.timestamp_ns, h.timestamp_ns);
    EXPECT_EQ(e.ref, 5u);
    EXPECT_EQ(e.shares, 40u);
    EXPECT_EQ(e.match, 777u);

    ASSERT_EQ(encode(OrderExecutedWithPrice{with('C'), 6, 10, 778, 'N', 123400}, buf), 36u);
    const auto c = decode_executed_with_price(buf);
    EXPECT_EQ(c.printable, 'N');
    EXPECT_EQ(c.price, 123400u);

    ASSERT_EQ(encode(OrderCancel{with('X'), 7, 25}, buf), 23u);
    EXPECT_EQ(decode_cancel(buf).shares, 25u);

    ASSERT_EQ(encode(OrderDelete{with('D'), 8}, buf), 19u);
    EXPECT_EQ(decode_delete(buf).ref, 8u);

    ASSERT_EQ(encode(OrderReplace{with('U'), 8, 9, 500, 990000}, buf), 35u);
    const auto u = decode_replace(buf);
    EXPECT_EQ(u.old_ref, 8u);
    EXPECT_EQ(u.new_ref, 9u);
    EXPECT_EQ(u.shares, 500u);
    EXPECT_EQ(u.price, 990000u);

    ASSERT_EQ(encode(Trade{with('P'), 0, 'B', 100, stock_name("QQQ"), 4000000, 779}, buf), 44u);
    const auto t = decode_trade(buf);
    EXPECT_EQ(t.match, 779u);
    EXPECT_EQ(t.stock, stock_name("QQQ"));

    ASSERT_EQ(encode(StockDirectory{with('R'), stock_name("ZVZZT"), 'Q', 'N', 100, 'N'}, buf), 39u);
    const auto r = decode_stock_directory(buf);
    EXPECT_EQ(r.stock, stock_name("ZVZZT"));
    EXPECT_EQ(r.round_lot_size, 100u);
}

TEST(Messages, TimestampUsesAllSixBytes) {
    std::byte buf[64];
    const uint64_t late = 86'399'999'999'999ull;  // 23:59:59.999999999 needs more than 32 bits
    encode(OrderDelete{{'D', 1, 0, late}, 1}, buf);
    EXPECT_EQ(decode_header(buf).timestamp_ns, late);
}

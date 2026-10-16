#include "itch/messages.hpp"

#include <algorithm>

namespace itch {
namespace {

void put_header(const Header& h, std::byte* p) {
    p[0] = std::byte(h.type);
    put16(p + 1, h.locate);
    put16(p + 3, h.tracking);
    put48(p + 5, h.timestamp_ns);
}

std::array<char, 8> get8(const std::byte* p) {
    std::array<char, 8> a;
    std::memcpy(a.data(), p, 8);
    return a;
}

}  // namespace

std::array<char, 8> stock_name(const char* s) {
    std::array<char, 8> a;
    a.fill(' ');
    for (std::size_t i = 0; i < 8 && s[i]; ++i) a[i] = s[i];
    return a;
}

// Offsets below follow the ITCH 5.0 specification; the 11-byte header is common to all.
AddOrder decode_add(const std::byte* p) {
    AddOrder m{decode_header(p), be64(p + 11), char(p[19]), be32(p + 20), get8(p + 24), be32(p + 32), std::nullopt};
    if (m.h.type == 'F') {
        std::array<char, 4> mpid;
        std::memcpy(mpid.data(), p + 36, 4);
        m.mpid = mpid;
    }
    return m;
}

OrderExecuted decode_executed(const std::byte* p) {
    return {decode_header(p), be64(p + 11), be32(p + 19), be64(p + 23)};
}

OrderExecutedWithPrice decode_executed_with_price(const std::byte* p) {
    return {decode_header(p), be64(p + 11), be32(p + 19), be64(p + 23), char(p[31]), be32(p + 32)};
}

OrderCancel decode_cancel(const std::byte* p) { return {decode_header(p), be64(p + 11), be32(p + 19)}; }

OrderDelete decode_delete(const std::byte* p) { return {decode_header(p), be64(p + 11)}; }

OrderReplace decode_replace(const std::byte* p) {
    return {decode_header(p), be64(p + 11), be64(p + 19), be32(p + 27), be32(p + 31)};
}

Trade decode_trade(const std::byte* p) {
    return {decode_header(p), be64(p + 11), char(p[19]), be32(p + 20), get8(p + 24), be32(p + 32), be64(p + 36)};
}

SystemEvent decode_system_event(const std::byte* p) { return {decode_header(p), char(p[11])}; }

StockDirectory decode_stock_directory(const std::byte* p) {
    return {decode_header(p), get8(p + 11), char(p[19]), char(p[20]), be32(p + 21), char(p[25])};
}

TradingAction decode_trading_action(const std::byte* p) { return {decode_header(p), get8(p + 11), char(p[19])}; }

std::size_t encode(const AddOrder& m, std::byte* p) {
    const bool mpid = m.h.type == 'F';
    put_header(m.h, p);
    put64(p + 11, m.ref);
    p[19] = std::byte(m.side);
    put32(p + 20, m.shares);
    std::memcpy(p + 24, m.stock.data(), 8);
    put32(p + 32, m.price);
    if (mpid) std::memcpy(p + 36, m.mpid ? m.mpid->data() : "    ", 4);
    return kMessageSize[uint8_t(m.h.type)];
}

std::size_t encode(const OrderExecuted& m, std::byte* p) {
    put_header(m.h, p);
    put64(p + 11, m.ref);
    put32(p + 19, m.shares);
    put64(p + 23, m.match);
    return kMessageSize['E'];
}

std::size_t encode(const OrderExecutedWithPrice& m, std::byte* p) {
    put_header(m.h, p);
    put64(p + 11, m.ref);
    put32(p + 19, m.shares);
    put64(p + 23, m.match);
    p[31] = std::byte(m.printable);
    put32(p + 32, m.price);
    return kMessageSize['C'];
}

std::size_t encode(const OrderCancel& m, std::byte* p) {
    put_header(m.h, p);
    put64(p + 11, m.ref);
    put32(p + 19, m.shares);
    return kMessageSize['X'];
}

std::size_t encode(const OrderDelete& m, std::byte* p) {
    put_header(m.h, p);
    put64(p + 11, m.ref);
    return kMessageSize['D'];
}

std::size_t encode(const OrderReplace& m, std::byte* p) {
    put_header(m.h, p);
    put64(p + 11, m.old_ref);
    put64(p + 19, m.new_ref);
    put32(p + 27, m.shares);
    put32(p + 31, m.price);
    return kMessageSize['U'];
}

std::size_t encode(const Trade& m, std::byte* p) {
    put_header(m.h, p);
    put64(p + 11, m.ref);
    p[19] = std::byte(m.side);
    put32(p + 20, m.shares);
    std::memcpy(p + 24, m.stock.data(), 8);
    put32(p + 32, m.price);
    put64(p + 36, m.match);
    return kMessageSize['P'];
}

std::size_t encode(const SystemEvent& m, std::byte* p) {
    put_header(m.h, p);
    p[11] = std::byte(m.event_code);
    return kMessageSize['S'];
}

std::size_t encode(const StockDirectory& m, std::byte* p) {
    std::fill(p, p + kMessageSize['R'], std::byte(' '));
    put_header(m.h, p);
    std::memcpy(p + 11, m.stock.data(), 8);
    p[19] = std::byte(m.market_category);
    p[20] = std::byte(m.financial_status);
    put32(p + 21, m.round_lot_size);
    p[25] = std::byte(m.round_lots_only);
    put32(p + 34, 0);  // ETP leverage factor
    return kMessageSize['R'];
}

std::size_t encode(const TradingAction& m, std::byte* p) {
    std::fill(p, p + kMessageSize['H'], std::byte(' '));
    put_header(m.h, p);
    std::memcpy(p + 11, m.stock.data(), 8);
    p[19] = std::byte(m.state);
    return kMessageSize['H'];
}

}  // namespace itch

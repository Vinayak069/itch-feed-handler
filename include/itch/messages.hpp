// NASDAQ TotalView-ITCH 5.0 messages: sizes, decoded structs, decode and encode.
//
// Every field is big-endian and messages are packed with no padding, so fields are read
// with memcpy + byte swap rather than by casting a pointer to a struct (which would be
// unaligned and undefined behaviour). Prices are integers with 4 implied decimals.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace itch {

// Message length in bytes (without the 2-byte length prefix used in files and MoldUDP64), by type.
// 0 means "not an ITCH 5.0 message type".
constexpr std::array<uint8_t, 256> kMessageSize = [] {
    std::array<uint8_t, 256> s{};
    s['S'] = 12; s['R'] = 39; s['H'] = 25; s['Y'] = 20; s['L'] = 26; s['V'] = 35; s['W'] = 12;
    s['K'] = 28; s['J'] = 35; s['h'] = 21; s['A'] = 36; s['F'] = 40; s['E'] = 31; s['C'] = 36;
    s['X'] = 23; s['D'] = 19; s['U'] = 35; s['P'] = 44; s['Q'] = 40; s['B'] = 19; s['I'] = 50;
    s['N'] = 20; s['O'] = 48;
    return s;
}();

// ---- big-endian field access -------------------------------------------------------------
inline uint16_t be16(const std::byte* p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }
inline uint32_t be32(const std::byte* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
inline uint64_t be64(const std::byte* p) { uint64_t v; std::memcpy(&v, p, 8); return __builtin_bswap64(v); }
inline uint64_t be48(const std::byte* p) {  // ITCH timestamps: nanoseconds since midnight in 6 bytes
    return (uint64_t(be16(p)) << 32) | be32(p + 2);
}
inline void put16(std::byte* p, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(p, &v, 2); }
inline void put32(std::byte* p, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(p, &v, 4); }
inline void put64(std::byte* p, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(p, &v, 8); }
inline void put48(std::byte* p, uint64_t v) { put16(p, uint16_t(v >> 32)); put32(p + 2, uint32_t(v)); }

// ---- decoded messages ----------------------------------------------------------------------
struct Header {
    char type;
    uint16_t locate;       // NASDAQ's per-day integer id for the stock
    uint16_t tracking;
    uint64_t timestamp_ns; // since midnight
};

struct SystemEvent { Header h; char event_code; };
struct StockDirectory { Header h; std::array<char, 8> stock; char market_category; char financial_status;
                        uint32_t round_lot_size; char round_lots_only; };
struct TradingAction { Header h; std::array<char, 8> stock; char state; };
struct AddOrder { Header h; uint64_t ref; char side; uint32_t shares; std::array<char, 8> stock; uint32_t price;
                  std::optional<std::array<char, 4>> mpid; };  // mpid set for type 'F'
struct OrderExecuted { Header h; uint64_t ref; uint32_t shares; uint64_t match; };
struct OrderExecutedWithPrice { Header h; uint64_t ref; uint32_t shares; uint64_t match; char printable;
                                uint32_t price; };
struct OrderCancel { Header h; uint64_t ref; uint32_t shares; };
struct OrderDelete { Header h; uint64_t ref; };
struct OrderReplace { Header h; uint64_t old_ref; uint64_t new_ref; uint32_t shares; uint32_t price; };
struct Trade { Header h; uint64_t ref; char side; uint32_t shares; std::array<char, 8> stock; uint32_t price;
               uint64_t match; };

inline Header decode_header(const std::byte* p) {
    return {char(p[0]), be16(p + 1), be16(p + 3), be48(p + 5)};
}

// Size check: returns the expected size for this type, or 0 if the type is unknown.
inline std::size_t expected_size(std::byte type) { return kMessageSize[uint8_t(type)]; }

// Decoders take a pointer to a message whose type and length have already been checked.
AddOrder decode_add(const std::byte* p);  // 'A' and 'F'
OrderExecuted decode_executed(const std::byte* p);
OrderExecutedWithPrice decode_executed_with_price(const std::byte* p);
OrderCancel decode_cancel(const std::byte* p);
OrderDelete decode_delete(const std::byte* p);
OrderReplace decode_replace(const std::byte* p);
Trade decode_trade(const std::byte* p);
SystemEvent decode_system_event(const std::byte* p);
StockDirectory decode_stock_directory(const std::byte* p);
TradingAction decode_trading_action(const std::byte* p);

// Encoders write exactly kMessageSize[type] bytes and return that count. Used by the
// synthetic feed generator and by tests.
std::size_t encode(const AddOrder& m, std::byte* out);
std::size_t encode(const OrderExecuted& m, std::byte* out);
std::size_t encode(const OrderExecutedWithPrice& m, std::byte* out);
std::size_t encode(const OrderCancel& m, std::byte* out);
std::size_t encode(const OrderDelete& m, std::byte* out);
std::size_t encode(const OrderReplace& m, std::byte* out);
std::size_t encode(const Trade& m, std::byte* out);
std::size_t encode(const SystemEvent& m, std::byte* out);
std::size_t encode(const StockDirectory& m, std::byte* out);
std::size_t encode(const TradingAction& m, std::byte* out);

std::array<char, 8> stock_name(const char* s);  // space-padded, as ITCH sends it

}  // namespace itch

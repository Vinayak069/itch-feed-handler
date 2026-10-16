// Synthetic ITCH 5.0 feed with a realistic message mix.
//
// NASDAQ's historical ITCH files are not reachable from every machine, so the benchmarks and
// tests run on a generated stream instead. The generator emits byte-exact ITCH 5.0 messages
// and keeps its own copy of every live order, so every Execute, Cancel, Delete and Replace
// refers to an order that really exists. Defaults follow the published message counts of a
// full NASDAQ day (30 Dec 2019: ~44% adds, ~43% deletes, ~8% replaces, ~3% executions,
// ~1% partial cancels), with about one million orders resting at once and activity spread
// across symbols by a Zipf law (a few names carry most of the flow).
//
// New orders are placed relative to the book the stream has built so far (kept with the same
// OrderBook class the handler uses), so a resting bid is never at or above the best offer.
//
// It is a model, not a recording: price paths, order lifetimes and queue positions are
// simple random processes. Run tools/itch_replay on a real NASDAQ file to measure on real data.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <memory>
#include <vector>

#include "book/order_book.hpp"

namespace itch {

struct GenConfig {
    uint64_t messages = 10'000'000;     // total messages, including the start-of-day directory
    uint32_t symbols = 3'000;
    uint64_t live_target = 1'000'000;   // resting orders the book holds in steady state
    double zipf_s = 1.0;                // activity skew across symbols
    uint64_t seed = 42;
    // Relative frequency of order messages after warm-up (per 1,000, from the 2019 day).
    double w_add = 436, w_add_mpid = 5.5, w_exec = 21.3, w_exec_price = 0.4, w_cancel = 10.4,
           w_delete = 425.7, w_replace = 80.5, w_trade = 4.5;
};

struct GenStats {
    std::array<uint64_t, 256> by_type{};
    uint64_t peak_live = 0;
};

class Generator {
public:
    explicit Generator(const GenConfig& cfg);

    // Writes the next message (without length prefix) to `out`, which must hold at least
    // 64 bytes. Returns its length, or 0 when the configured number of messages is done.
    std::size_t next(std::byte* out);

    const GenStats& stats() const { return stats_; }
    std::size_t live_orders() const { return live_.size(); }

private:
    struct Live {
        uint64_t ref;
        uint32_t price;
        uint32_t shares;
        uint16_t locate;
        bool is_buy;
    };
    struct Sym {
        uint32_t base;  // opening price, 1/10,000 dollars on a one-cent grid
        uint32_t mid;   // reference price: random walk within a band around base
        std::array<char, 8> name;
    };

    std::size_t emit_add(std::byte* out, bool mpid);
    std::size_t emit_exec(std::byte* out, bool with_price);
    std::size_t emit_cancel(std::byte* out);
    std::size_t emit_delete(std::byte* out);
    std::size_t emit_replace(std::byte* out);
    std::size_t emit_trade(std::byte* out);
    uint16_t pick_symbol();
    std::size_t pick_live();            // index into live_, biased to recent orders
    void drop_live(std::size_t i);
    uint32_t quote_price(Sym& s, bool is_buy);
    uint32_t lot();
    uint64_t tick_time();

    GenConfig cfg_;
    std::mt19937_64 rng_;
    std::vector<double> sym_cdf_;
    std::vector<Sym> syms_;
    std::vector<Live> live_;
    std::discrete_distribution<int> mix_;
    uint64_t emitted_ = 0;
    uint64_t next_ref_ = 1;
    uint64_t next_match_ = 1;
    uint64_t clock_ns_ = 0;
    uint64_t step_ns_ = 0;
    uint32_t directory_sent_ = 0;
    int phase_ = 0;  // 0 start-of-day, 1 directory, 2 market open, 3 order flow, 4 end-of-day
    bool warm_ = false;
    GenStats stats_;
    std::unique_ptr<book::OrderBook> book_;  // the stream so far, so new orders never cross the book
};

// Convenience: the whole stream in file framing (2-byte length prefix per message).
std::vector<std::byte> generate_framed(const GenConfig& cfg, GenStats* stats = nullptr);

}  // namespace itch

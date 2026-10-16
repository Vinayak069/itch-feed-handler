#include "itch/generator.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "itch/messages.hpp"

namespace itch {
namespace {

constexpr uint64_t kSecond = 1'000'000'000ull;
constexpr uint64_t kOpen = 9 * 3600 * kSecond + 30 * 60 * kSecond;
constexpr uint64_t kClose = 16 * 3600 * kSecond;
constexpr uint32_t kCent = 100;          // prices carry 4 implied decimals
constexpr std::size_t kRecent = 4096;    // most cancels hit orders added moments ago

std::array<char, 8> symbol_name(uint32_t i) {
    std::string s;
    do {
        s.insert(s.begin(), char('A' + i % 26));
        i /= 26;
    } while (i > 0 && s.size() < 7);
    while (s.size() < 4) s.insert(s.begin(), 'Z');
    return stock_name(s.c_str());
}

}  // namespace

Generator::Generator(const GenConfig& cfg)
    : cfg_(cfg),
      rng_(cfg.seed),
      mix_({cfg.w_add, cfg.w_add_mpid, cfg.w_exec, cfg.w_exec_price, cfg.w_cancel, cfg.w_delete, cfg.w_replace,
            cfg.w_trade}) {
    if (cfg_.symbols == 0 || cfg_.symbols > 65000) throw std::invalid_argument("symbols must be in [1, 65000]");
    if (cfg_.messages < cfg_.symbols + 3ull) throw std::invalid_argument("too few messages for the directory");

    double total = 0;
    sym_cdf_.reserve(cfg_.symbols);
    for (uint32_t i = 0; i < cfg_.symbols; ++i) sym_cdf_.push_back(total += 1.0 / std::pow(double(i + 1), cfg_.zipf_s));
    for (double& c : sym_cdf_) c /= total;

    // Prices log-uniform between $5 and $500.
    std::uniform_real_distribution<double> logp(std::log(5.0), std::log(500.0));
    syms_.reserve(cfg_.symbols);
    for (uint32_t i = 0; i < cfg_.symbols; ++i) {
        const double dollars = std::exp(logp(rng_));
        const uint32_t px = uint32_t(std::lround(dollars * 100)) * kCent;
        syms_.push_back({px, px, symbol_name(i)});
    }
    book_ = std::make_unique<book::OrderBook>(cfg_.live_target + 1024);
    live_.reserve(cfg_.live_target + cfg_.live_target / 8 + 16);

    const uint64_t flow = cfg_.messages - cfg_.symbols - 3;
    step_ns_ = std::max<uint64_t>(1, (kClose - kOpen) / std::max<uint64_t>(1, flow));
    clock_ns_ = 4 * 3600 * kSecond;
}

uint16_t Generator::pick_symbol() {
    const double u = std::uniform_real_distribution<double>(0, 1)(rng_);
    const auto it = std::lower_bound(sym_cdf_.begin(), sym_cdf_.end(), u);
    return uint16_t(std::min<std::size_t>(std::size_t(it - sym_cdf_.begin()), cfg_.symbols - 1) + 1);  // locate >= 1
}

std::size_t Generator::pick_live() {
    const std::size_t n = live_.size();
    if (std::uniform_int_distribution<int>(0, 3)(rng_) != 0) {  // 75%: one of the newest orders
        const std::size_t k = std::min(n, kRecent);
        return n - 1 - std::uniform_int_distribution<std::size_t>(0, k - 1)(rng_);
    }
    return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng_);
}

void Generator::drop_live(std::size_t i) {
    live_[i] = live_.back();
    live_.pop_back();
}

uint32_t Generator::quote_price(Sym& s, bool is_buy) {
    // The reference price drifts a cent at a time inside a band of +-kBand cents around the
    // opening price; new orders rest a few cents from it, and never cross the opposite side.
    constexpr uint32_t kBand = 25 * kCent;
    if (std::uniform_int_distribution<int>(0, 19)(rng_) == 0) {
        const bool up = std::uniform_int_distribution<int>(0, 1)(rng_) == 1;
        if (up && s.mid + kCent <= s.base + kBand) s.mid += kCent;
        else if (!up && s.mid >= s.base - kBand + kCent) s.mid -= kCent;  // base >= $5, so this stays positive
    }
    const uint32_t depth = std::min<uint32_t>(std::geometric_distribution<uint32_t>(0.35)(rng_), 50);
    const uint16_t locate = uint16_t(&s - syms_.data() + 1);
    if (is_buy) {
        uint32_t px = s.mid > (depth + 1) * kCent ? s.mid - depth * kCent : kCent;
        if (const auto ask = book_->best_ask(locate); ask && px >= ask->price) px = ask->price - kCent;
        return std::max(px, kCent);
    }
    uint32_t px = s.mid + kCent + depth * kCent;
    if (const auto bid = book_->best_bid(locate); bid && px <= bid->price) px = bid->price + kCent;
    return px;
}

uint32_t Generator::lot() {
    const int r = std::uniform_int_distribution<int>(0, 9)(rng_);
    if (r < 7) return 100;
    if (r < 9) return 100 * std::uniform_int_distribution<uint32_t>(2, 10)(rng_);
    return std::uniform_int_distribution<uint32_t>(1, 99)(rng_);  // odd lot
}

uint64_t Generator::tick_time() { return clock_ns_ += step_ns_; }

std::size_t Generator::emit_add(std::byte* out, bool mpid) {
    const uint16_t locate = pick_symbol();
    Sym& s = syms_[locate - 1];
    const bool is_buy = std::uniform_int_distribution<int>(0, 1)(rng_) == 1;
    const Live o{next_ref_++, quote_price(s, is_buy), lot(), locate, is_buy};
    live_.push_back(o);
    AddOrder m{{mpid ? 'F' : 'A', locate, 0, tick_time()}, o.ref, is_buy ? 'B' : 'S', o.shares, s.name, o.price,
               std::nullopt};
    if (mpid) m.mpid = std::array<char, 4>{'M', 'P', 'I', 'D'};
    return encode(m, out);
}

std::size_t Generator::emit_exec(std::byte* out, bool with_price) {
    const std::size_t i = pick_live();
    Live& o = live_[i];
    const bool full = o.shares == 1 || std::uniform_int_distribution<int>(0, 9)(rng_) < 6;
    const uint32_t n = full ? o.shares : std::uniform_int_distribution<uint32_t>(1, o.shares - 1)(rng_);
    const Header h{with_price ? 'C' : 'E', o.locate, 0, tick_time()};
    std::size_t len;
    if (with_price) len = encode(OrderExecutedWithPrice{h, o.ref, n, next_match_++, 'Y', o.price}, out);
    else len = encode(OrderExecuted{h, o.ref, n, next_match_++}, out);
    if (full) drop_live(i);
    else o.shares -= n;
    return len;
}

std::size_t Generator::emit_cancel(std::byte* out) {
    const std::size_t i = pick_live();
    Live& o = live_[i];
    if (o.shares < 2) return emit_delete(out);
    const uint32_t n = std::uniform_int_distribution<uint32_t>(1, o.shares - 1)(rng_);
    o.shares -= n;
    return encode(OrderCancel{{'X', o.locate, 0, tick_time()}, o.ref, n}, out);
}

std::size_t Generator::emit_delete(std::byte* out) {
    const std::size_t i = pick_live();
    const Live o = live_[i];
    drop_live(i);
    return encode(OrderDelete{{'D', o.locate, 0, tick_time()}, o.ref}, out);
}

std::size_t Generator::emit_replace(std::byte* out) {
    Live& o = live_[pick_live()];
    const uint64_t old_ref = o.ref;
    o.ref = next_ref_++;
    o.price = quote_price(syms_[o.locate - 1], o.is_buy);
    o.shares = lot();
    return encode(OrderReplace{{'U', o.locate, 0, tick_time()}, old_ref, o.ref, o.shares, o.price}, out);
}

std::size_t Generator::emit_trade(std::byte* out) {
    const uint16_t locate = pick_symbol();
    const Sym& s = syms_[locate - 1];
    return encode(Trade{{'P', locate, 0, tick_time()}, 0, 'B', lot(), s.name, s.mid, next_match_++}, out);
}

std::size_t Generator::next(std::byte* out) {
    if (emitted_ >= cfg_.messages) return 0;
    std::size_t len = 0;
    if (phase_ == 0) {  // start of messages
        len = encode(SystemEvent{{'S', 0, 0, clock_ns_}, 'O'}, out);
        phase_ = 1;
    } else if (phase_ == 1) {  // one directory entry per stock, locate codes 1..N
        const uint32_t i = directory_sent_++;
        len = encode(StockDirectory{{'R', uint16_t(i + 1), 0, clock_ns_}, syms_[i].name, 'Q', 'N', 100, 'N'}, out);
        if (directory_sent_ == cfg_.symbols) phase_ = 2;
    } else if (phase_ == 2) {  // start of market hours
        clock_ns_ = kOpen;
        len = encode(SystemEvent{{'S', 0, 0, clock_ns_}, 'Q'}, out);
        phase_ = 3;
    } else if (emitted_ + 1 == cfg_.messages) {  // end of market hours, last message
        len = encode(SystemEvent{{'S', 0, 0, std::max(clock_ns_, kClose)}, 'M'}, out);
    } else {
        if (!warm_ && live_.size() >= cfg_.live_target) warm_ = true;
        int kind = 0;  // 0 A, 1 F, 2 E, 3 C, 4 X, 5 D, 6 U, 7 P
        if (warm_) {
            kind = mix_(rng_);
            // Keep the resting book near its target size: the 2019 mix is almost balanced,
            // and this nudges it back when random drift pushes it away.
            const uint64_t n = live_.size();
            if (kind <= 1 && n > cfg_.live_target + cfg_.live_target / 20) kind = 5;
            else if (kind == 5 && n + cfg_.live_target / 20 < cfg_.live_target) kind = 0;
        }
        if (kind >= 2 && kind <= 6 && live_.empty()) kind = 0;
        switch (kind) {
            case 0: len = emit_add(out, false); break;
            case 1: len = emit_add(out, true); break;
            case 2: len = emit_exec(out, false); break;
            case 3: len = emit_exec(out, true); break;
            case 4: len = emit_cancel(out); break;
            case 5: len = emit_delete(out); break;
            case 6: len = emit_replace(out); break;
            default: len = emit_trade(out); break;
        }
    }
    book_->apply(out);
    ++emitted_;
    ++stats_.by_type[uint8_t(out[0])];
    stats_.peak_live = std::max<uint64_t>(stats_.peak_live, live_.size());
    return len;
}

std::vector<std::byte> generate_framed(const GenConfig& cfg, GenStats* stats) {
    Generator g(cfg);
    std::vector<std::byte> out;
    out.reserve(cfg.messages * 30);
    std::byte msg[64];
    while (const std::size_t len = g.next(msg)) {
        const std::size_t at = out.size();
        out.resize(at + 2 + len);
        put16(out.data() + at, uint16_t(len));
        std::memcpy(out.data() + at + 2, msg, len);
    }
    if (stats) *stats = g.stats();
    return out;
}

}  // namespace itch

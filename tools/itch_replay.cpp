// Replays an ITCH 5.0 file (NASDAQ's framing, e.g. 12302019.NASDAQ_ITCH50 after gunzip, or
// itch_gen output) through the parser and order book and reports throughput.
//   itch_replay --file feed.itch [--prefetch 8] [--no-huge-pages] [--latency] [--chunk-mb 64]
// Reads in chunks and times only parsing + book updates, not disk reads.
// --prefetch K applies each message K messages after it was parsed, prefetching its order
// slot in between (0 turns this off). --latency also times every message individually (adds
// ~2 clock reads per message to the measured cost).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <iostream>
#include <vector>

#include "args.hpp"
#include "book/order_book.hpp"
#include "itch/framing.hpp"
#include "util/clock.hpp"
#include "util/histogram.hpp"

int main(int argc, char** argv) try {
    const Args a(argc, argv);
    const std::string path = a.str("file", "feed.itch");
    const bool latency = a.has("latency");
    const std::size_t chunk = std::size_t(a.num("chunk-mb", 64)) << 20;
    const std::size_t ahead = std::size_t(a.num("prefetch", 8));
    if (ahead > 64) throw std::invalid_argument("--prefetch at most 64");
    book::OrderStore::use_huge_pages = !a.has("no-huge-pages");
    const bool parse_only = a.has("parse-only");  // framing + decode, no book: the floor

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<std::byte> buf(chunk + (1 << 16));
    auto book = std::make_unique<book::OrderBook>(uint64_t(a.num("expected-orders", 1 << 21)));
    util::Histogram per_msg;
    std::size_t carry = 0;
    int64_t busy_ns = 0;
    uint64_t messages = 0, peak_live = 0;
    for (;;) {
        const std::size_t got = std::fread(buf.data() + carry, 1, chunk, f);
        if (got == 0) break;
        const std::span<const std::byte> data(buf.data(), carry + got);
        const int64_t t0 = util::now_ns();
        uint64_t sink = 0;
        auto apply = [&](const std::byte* m) {
            if (parse_only) {
                sink += itch::decode_header(m).timestamp_ns;
            } else if (latency) {
                const int64_t s = util::now_ns();
                book->apply(m);
                per_msg.record(util::now_ns() - s);
            } else {
                book->apply(m);
            }
        };
        // Software pipeline: prefetch message i, apply message i - ahead. Pointers stay valid
        // because the whole chunk is in memory until the chunk's messages are all applied.
        const std::byte* window[64];
        std::size_t in_window = 0, pos = 0;
        const itch::FrameResult r = itch::for_each_framed(data, [&](const std::byte* m, std::size_t) {
            if (ahead == 0 || parse_only) return apply(m);
            book->prefetch(m);
            if (in_window == ahead) apply(window[pos]);
            else ++in_window;
            window[pos] = m;
            pos = (pos + 1) % ahead;
        });
        for (std::size_t i = 0; i < in_window; ++i) apply(window[(pos + ahead - in_window + i) % ahead]);
        busy_ns += util::now_ns() - t0;
        if (sink == 1) std::cerr << "";  // keeps the parse-only loop from being optimised away
        messages += r.messages;
        peak_live = std::max<uint64_t>(peak_live, book->live_orders());
        if (r.error != itch::FrameError::None) throw std::runtime_error("malformed message after " + std::to_string(messages));
        carry = data.size() - r.consumed;
        std::memmove(buf.data(), buf.data() + r.consumed, carry);
    }
    std::fclose(f);
    if (carry) std::cerr << "itch_replay: " << carry << " trailing bytes (truncated last message)\n";

    const auto& s = book->stats();
    const double sec = double(busy_ns) / 1e9;
    std::cout << "{\"file\": \"" << path << "\", \"prefetch\": " << ahead
              << ", \"huge_pages\": " << (book::OrderStore::use_huge_pages ? "true" : "false") << ", \"messages\": " << messages << ", \"seconds\": " << sec
              << ", \"messages_per_sec\": " << double(messages) / sec << ", \"ns_per_message\": " << double(busy_ns) / double(messages)
              << ", \"adds\": " << s.adds << ", \"executes\": " << s.executes << ", \"cancels\": " << s.cancels
              << ", \"deletes\": " << s.deletes << ", \"replaces\": " << s.replaces << ", \"other\": " << s.other
              << ", \"unknown_ref\": " << s.unknown_ref << ", \"duplicate_ref\": " << s.duplicate_ref
              << ", \"top_of_book_changes\": " << s.top_changes << ", \"live_orders_end\": " << book->live_orders()
              << ", \"live_orders_peak_at_chunk_end\": " << peak_live;
    if (latency) std::cout << ", \"per_message_ns\": " << per_msg.summary_json();
    std::cout << "}\n";
} catch (const std::exception& e) {
    std::cerr << "itch_replay: " << e.what() << "\n";
    return 1;
}

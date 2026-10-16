// Writes a synthetic ITCH 5.0 stream in NASDAQ's file framing (2-byte length prefix).
//   itch_gen --out feed.itch [--messages 100000000] [--symbols 3000] [--live 1000000] [--seed 42]
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include "args.hpp"
#include "itch/generator.hpp"
#include "itch/messages.hpp"

int main(int argc, char** argv) try {
    const Args a(argc, argv);
    itch::GenConfig cfg;
    cfg.messages = uint64_t(a.num("messages", 10e6));
    cfg.symbols = uint32_t(a.num("symbols", 3000));
    cfg.live_target = uint64_t(a.num("live", 1e6));
    cfg.seed = uint64_t(a.num("seed", 42));
    const std::string path = a.str("out", "feed.itch");

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot open " + path);
    itch::Generator g(cfg);
    std::vector<std::byte> buf;
    buf.reserve(1 << 22);
    std::byte msg[64];
    uint64_t bytes = 0;
    while (const std::size_t len = g.next(msg)) {
        const std::size_t at = buf.size();
        buf.resize(at + 2 + len);
        itch::put16(buf.data() + at, uint16_t(len));
        std::memcpy(buf.data() + at + 2, msg, len);
        if (buf.size() > (1 << 22) - 64) {
            bytes += std::fwrite(buf.data(), 1, buf.size(), f);
            buf.clear();
        }
    }
    bytes += std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);

    const auto& s = g.stats();
    std::cout << "{\"file\": \"" << path << "\", \"bytes\": " << bytes << ", \"peak_live_orders\": " << s.peak_live
              << ", \"by_type\": {";
    bool first = true;
    for (int t = 0; t < 256; ++t)
        if (s.by_type[t]) {
            std::cout << (first ? "" : ", ") << "\"" << char(t) << "\": " << s.by_type[t];
            first = false;
        }
    std::cout << "}}\n";
} catch (const std::exception& e) {
    std::cerr << "itch_gen: " << e.what() << "\n";
    return 1;
}

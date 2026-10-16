// A deliberately simple order book (std::map everywhere) used as the oracle in tests.
#pragma once

#include <map>
#include <optional>
#include <unordered_map>

#include "itch/messages.hpp"

struct RefBook {
    struct O { uint16_t locate; bool buy; uint32_t price, shares; };
    struct Lvl { uint64_t shares = 0; uint32_t orders = 0; };
    std::unordered_map<uint64_t, O> orders;
    std::map<uint16_t, std::map<uint32_t, Lvl>> bids, asks;

    std::map<uint32_t, Lvl>& side(const O& o) { return (o.buy ? bids : asks)[o.locate]; }

    void add(uint64_t ref, const O& o) {
        orders[ref] = o;
        auto& l = side(o)[o.price];
        l.shares += o.shares;
        ++l.orders;
    }
    void reduce(uint64_t ref, uint32_t n) {
        auto it = orders.find(ref);
        if (it == orders.end()) return;
        O& o = it->second;
        n = std::min(n, o.shares);
        auto& lv = side(o);
        auto& l = lv[o.price];
        l.shares -= n;
        o.shares -= n;
        if (o.shares == 0) {
            if (--l.orders == 0) lv.erase(o.price);
            orders.erase(it);
        }
    }
    void remove(uint64_t ref) {
        auto it = orders.find(ref);
        if (it != orders.end()) reduce(ref, it->second.shares);
    }

    void apply(const std::byte* p) {
        using namespace itch;
        switch (char(p[0])) {
            case 'A': case 'F': {
                const auto m = decode_add(p);
                add(m.ref, {m.h.locate, m.side == 'B', m.price, m.shares});
                break;
            }
            case 'E': { const auto m = decode_executed(p); reduce(m.ref, m.shares); break; }
            case 'C': { const auto m = decode_executed_with_price(p); reduce(m.ref, m.shares); break; }
            case 'X': { const auto m = decode_cancel(p); reduce(m.ref, m.shares); break; }
            case 'D': remove(decode_delete(p).ref); break;
            case 'U': {
                const auto m = decode_replace(p);
                auto it = orders.find(m.old_ref);
                if (it == orders.end()) break;
                const O o = it->second;
                remove(m.old_ref);
                add(m.new_ref, {o.locate, o.buy, m.price, m.shares});
                break;
            }
            default: break;
        }
    }

    std::optional<std::pair<uint32_t, uint64_t>> best(uint16_t locate, bool buy) {
        auto& m = buy ? bids : asks;
        auto it = m.find(locate);
        if (it == m.end() || it->second.empty()) return std::nullopt;
        const auto& lv = it->second;
        const auto best = buy ? std::prev(lv.end()) : lv.begin();
        return std::make_pair(best->first, best->second.shares);
    }
};

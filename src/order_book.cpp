#include "book/order_book.hpp"

#include <algorithm>

#include "itch/messages.hpp"

namespace book {

std::vector<Level>::iterator SideBook::find_level(uint32_t price) {
    // first level that is not worse than `price`; levels are sorted worst -> best
    return std::lower_bound(levels_.begin(), levels_.end(), price,
                            [this](const Level& l, uint32_t p) { return worse(l.price, p); });
}

void SideBook::add(uint32_t price, uint64_t shares) {
    if (!levels_.empty() && levels_.back().price == price) {  // fast path: joining the best level
        levels_.back().shares += shares;
        ++levels_.back().orders;
        return;
    }
    auto it = find_level(price);
    if (it != levels_.end() && it->price == price) {
        it->shares += shares;
        ++it->orders;
    } else {
        levels_.insert(it, Level{price, 1, shares});
    }
}

void SideBook::reduce(uint32_t price, uint64_t shares, bool remove_order) {
    auto it = (!levels_.empty() && levels_.back().price == price) ? levels_.end() - 1 : find_level(price);
    // The order store guarantees the level exists; a missing level is a bug, not bad input.
    it->shares -= shares;
    if (remove_order) --it->orders;
    if (it->orders == 0) levels_.erase(it);
}

bool OrderBook::touches_top(const StockBook& b, bool is_buy, uint32_t price) const {
    const auto best = is_buy ? b.bids.best() : b.asks.best();
    if (!best) return true;
    return is_buy ? price >= best->price : price <= best->price;
}

bool OrderBook::add(uint64_t ref, uint16_t locate, bool is_buy, uint32_t shares, uint32_t price) {
    if (!orders_.insert(Order{ref, price, shares, locate, is_buy})) {
        ++stats_.duplicate_ref;
        return false;
    }
    StockBook& b = book_for(locate);
    const bool top = touches_top(b, is_buy, price);
    (is_buy ? b.bids : b.asks).add(price, shares);
    return top;
}

bool OrderBook::execute(uint64_t ref, uint32_t shares) {
    Order* o = orders_.find(ref);
    if (!o) {
        ++stats_.unknown_ref;
        return false;
    }
    StockBook& b = *books_[o->locate];
    const bool top = touches_top(b, o->is_buy, o->price);
    const uint32_t n = std::min(shares, o->shares);
    const bool gone = n == o->shares;
    (o->is_buy ? b.bids : b.asks).reduce(o->price, n, gone);
    if (gone) orders_.erase(ref);
    else o->shares -= n;
    return top;
}

bool OrderBook::remove(uint64_t ref) {
    Order* o = orders_.find(ref);
    if (!o) {
        ++stats_.unknown_ref;
        return false;
    }
    StockBook& b = *books_[o->locate];
    const bool top = touches_top(b, o->is_buy, o->price);
    (o->is_buy ? b.bids : b.asks).reduce(o->price, o->shares, true);
    orders_.erase(ref);
    return top;
}

bool OrderBook::replace(uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price) {
    // A replace keeps the side and stock but loses time priority: delete + add under a new reference.
    Order* o = orders_.find(old_ref);
    if (!o) {
        ++stats_.unknown_ref;
        return false;
    }
    const uint16_t locate = o->locate;
    const bool is_buy = o->is_buy;
    const bool a = remove(old_ref);
    const bool b = add(new_ref, locate, is_buy, shares, price);
    return a || b;
}

std::optional<Quote> OrderBook::best_bid(uint16_t locate) const {
    return books_[locate] ? books_[locate]->bids.best() : std::nullopt;
}

std::optional<Quote> OrderBook::best_ask(uint16_t locate) const {
    return books_[locate] ? books_[locate]->asks.best() : std::nullopt;
}

void OrderBook::prefetch(const std::byte* p) const {
    switch (char(p[0])) {
        case 'U':
            orders_.prefetch(itch::be64(p + 19));  // the new reference's slot, then fall through
            [[fallthrough]];
        case 'A': case 'F': case 'E': case 'C': case 'X': case 'D':
            orders_.prefetch(itch::be64(p + 11));  // ref (old ref for U) is at offset 11 in all of them
            break;
        default:
            return;
    }
    if (const auto& b = books_[itch::be16(p + 1)]) __builtin_prefetch(b.get());
}

bool OrderBook::apply(const std::byte* p) {
    ++stats_.messages;
    bool top = false;
    switch (char(p[0])) {
        case 'A':
        case 'F': {
            const auto m = itch::decode_add(p);
            ++stats_.adds;
            top = add(m.ref, m.h.locate, m.side == 'B', m.shares, m.price);
            break;
        }
        case 'E': {
            const auto m = itch::decode_executed(p);
            ++stats_.executes;
            top = execute(m.ref, m.shares);
            break;
        }
        case 'C': {
            const auto m = itch::decode_executed_with_price(p);
            ++stats_.executes;
            top = execute(m.ref, m.shares);
            break;
        }
        case 'X': {
            const auto m = itch::decode_cancel(p);
            ++stats_.cancels;
            top = execute(m.ref, m.shares);
            break;
        }
        case 'D': {
            const auto m = itch::decode_delete(p);
            ++stats_.deletes;
            top = remove(m.ref);
            break;
        }
        case 'U': {
            const auto m = itch::decode_replace(p);
            ++stats_.replaces;
            top = replace(m.old_ref, m.new_ref, m.shares, m.price);
            break;
        }
        default:
            ++stats_.other;  // trades against hidden orders, directory, status, imbalance...
    }
    stats_.top_changes += top;
    return top;
}

}  // namespace book

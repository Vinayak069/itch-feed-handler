// Full-depth books for every stock on the feed, rebuilt from ITCH order messages.
//
// NASDAQ publishes changes, not the book: Add, Execute, Cancel (partial), Delete and Replace.
// Each order lives in the OrderStore (by reference number); each stock's book keeps
// aggregate price levels per side. A side is a vector of levels sorted so the best price is
// at the back: most activity is near the touch, so inserts and erases there move few elements
// and the best price is a single read.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "book/order_store.hpp"

namespace book {

struct Level {
    uint32_t price;
    uint32_t orders;
    uint64_t shares;
};

struct Quote {
    uint32_t price;
    uint64_t shares;
};

class SideBook {
public:
    explicit SideBook(bool is_buy) : is_buy_(is_buy) {}
    void add(uint32_t price, uint64_t shares);                 // one new order
    void reduce(uint32_t price, uint64_t shares, bool remove_order);
    std::optional<Quote> best() const {
        if (levels_.empty()) return std::nullopt;
        return Quote{levels_.back().price, levels_.back().shares};
    }
    const std::vector<Level>& levels() const { return levels_; }  // worst ... best

private:
    // true if a is a worse price than b for this side (worse sorts first)
    bool worse(uint32_t a, uint32_t b) const { return is_buy_ ? a < b : a > b; }
    std::vector<Level>::iterator find_level(uint32_t price);
    bool is_buy_;
    std::vector<Level> levels_;
};

struct StockBook {
    SideBook bids{true};
    SideBook asks{false};
};

struct BookStats {
    uint64_t messages = 0, adds = 0, executes = 0, cancels = 0, deletes = 0, replaces = 0, other = 0;
    uint64_t unknown_ref = 0;     // order messages for references we never saw added
    uint64_t duplicate_ref = 0;
    uint64_t top_changes = 0;     // messages that changed a best bid or offer (price or size)
};

class OrderBook {
public:
    explicit OrderBook(std::size_t expected_orders = 1 << 20) : orders_(expected_orders * 2) {}

    // Applies one ITCH message (any type; non-book messages are counted and ignored).
    // Returns true if it changed the best bid or offer of its stock.
    bool apply(const std::byte* msg);

    // Starts loading what apply(msg) will touch: the order's hash slot and the stock's book.
    // Called a few messages ahead, it overlaps those cache misses with useful work.
    void prefetch(const std::byte* msg) const;

    // Direct entry points, also used by tests.
    bool add(uint64_t ref, uint16_t locate, bool is_buy, uint32_t shares, uint32_t price);
    bool execute(uint64_t ref, uint32_t shares);  // also used for partial cancels
    bool remove(uint64_t ref);
    bool replace(uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price);

    const StockBook* stock(uint16_t locate) const { return books_[locate].get(); }
    std::optional<Quote> best_bid(uint16_t locate) const;
    std::optional<Quote> best_ask(uint16_t locate) const;
    std::size_t live_orders() const { return orders_.size(); }
    const BookStats& stats() const { return stats_; }

private:
    StockBook& book_for(uint16_t locate) {
        auto& b = books_[locate];
        if (!b) b = std::make_unique<StockBook>();
        return *b;
    }
    bool touches_top(const StockBook& b, bool is_buy, uint32_t price) const;

    OrderStore orders_;
    std::unique_ptr<StockBook> books_[65536];  // indexed by NASDAQ's stock locate code
    BookStats stats_;
};

}  // namespace book

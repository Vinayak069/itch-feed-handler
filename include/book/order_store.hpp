// Open-addressing hash table from ITCH order reference number to the live order.
//
// Executions, cancels and deletes carry only the reference number, so every one of them
// starts with a lookup here: this table is the hot path of the feed handler. Design:
// - one flat array of 24-byte slots, so a lookup touches one cache line in the common case;
// - linear probing with a multiplicative (Fibonacci) hash, because NASDAQ assigns reference
//   numbers almost sequentially and an identity hash would cluster them;
// - backward-shift deletion, so there are no tombstones and probe lengths stay short in a
//   table where most orders are deleted soon after they are added;
// - the slot array is 2 MB aligned and marked MADV_HUGEPAGE: with a million live orders it
//   spans ~100 MB, and with 4 KB pages nearly every lookup would also miss the TLB;
// - prefetch(ref) lets the caller start the cache miss for a message it will process soon.
#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>

namespace book {

struct Order {
    uint64_t ref;
    uint32_t price;
    uint32_t shares;
    uint16_t locate;
    bool is_buy;
};
static_assert(sizeof(Order) == 24);

class OrderStore {
public:
    static constexpr uint64_t kEmpty = ~uint64_t{0};

    explicit OrderStore(std::size_t initial_capacity = 1 << 16) { allocate(round_up(initial_capacity)); }

    // Huge pages are on by default; the switch exists so the benchmark can measure them.
    static inline bool use_huge_pages = true;

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return mask_ + 1; }

    // Returns nullptr if absent. The pointer is valid until the next insert or erase.
    Order* find(uint64_t ref) {
        for (std::size_t i = slot(ref);; i = (i + 1) & mask_) {
            if (slots_[i].ref == ref) return &slots_[i];
            if (slots_[i].ref == kEmpty) return nullptr;
        }
    }

    void prefetch(uint64_t ref) const { __builtin_prefetch(&slots_[slot(ref)]); }

    // Inserts a new order; returns false (and changes nothing) if the reference already exists.
    bool insert(const Order& o) {
        if (o.ref == kEmpty) throw std::invalid_argument("reserved reference number");
        if ((size_ + 1) * 2 > capacity()) grow();  // keep load factor <= 0.5
        for (std::size_t i = slot(o.ref);; i = (i + 1) & mask_) {
            if (slots_[i].ref == o.ref) return false;
            if (slots_[i].ref == kEmpty) {
                slots_[i] = o;
                ++size_;
                return true;
            }
        }
    }

    // Removes the order; returns false if absent.
    bool erase(uint64_t ref) {
        std::size_t i = slot(ref);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].ref == ref) break;
            if (slots_[i].ref == kEmpty) return false;
        }
        // Backward-shift: pull later entries of the probe run into the hole when their home
        // slot is at or before it, so lookups never need tombstones.
        std::size_t hole = i;
        for (std::size_t j = (hole + 1) & mask_;; j = (j + 1) & mask_) {
            if (slots_[j].ref == kEmpty) break;
            const std::size_t home = slot(slots_[j].ref);
            // is `home` cyclically outside (hole, j]? then the entry may move to the hole
            const bool movable = (hole <= j) ? (home <= hole || home > j) : (home <= hole && home > j);
            if (movable) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].ref = kEmpty;
        --size_;
        return true;
    }

private:
    static std::size_t round_up(std::size_t n) {
        std::size_t c = 16;
        while (c < n) c <<= 1;
        return c;
    }
    std::size_t slot(uint64_t ref) const { return std::size_t((ref * 0x9E3779B97F4A7C15ull) >> shift_); }

    struct Free {
        void operator()(Order* p) const { std::free(p); }
    };

    void allocate(std::size_t cap) {
        constexpr std::size_t kHuge = 2u << 20;
        const std::size_t bytes = (cap * sizeof(Order) + kHuge - 1) / kHuge * kHuge;
        auto* mem = static_cast<Order*>(std::aligned_alloc(kHuge, bytes));
        if (!mem) throw std::bad_alloc();
        if (use_huge_pages) ::madvise(mem, bytes, MADV_HUGEPAGE);  // a hint: harmless if unsupported
        slots_.reset(mem);
        for (std::size_t i = 0; i < cap; ++i) slots_[i].ref = kEmpty;
        mask_ = cap - 1;
        shift_ = 64 - unsigned(__builtin_ctzll(cap));
    }

    void grow() {
        auto old = std::move(slots_);
        const std::size_t old_cap = mask_ + 1;
        allocate(old_cap * 2);
        size_ = 0;
        for (std::size_t i = 0; i < old_cap; ++i)
            if (old[i].ref != kEmpty) insert(old[i]);
    }

    std::unique_ptr<Order[], Free> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
    std::size_t size_ = 0;
};

}  // namespace book

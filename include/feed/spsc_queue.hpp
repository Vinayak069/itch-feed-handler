// Bounded lock-free single-producer / single-consumer ring buffer.
//
// Used between the network thread (producer) and the book thread (consumer). Design points:
// - Capacity is a power of two, so wrapping is a mask, not a modulo.
// - head_ (written by the consumer) and tail_ (written by the producer) sit on separate
//   cache lines. If they shared one, every push would invalidate the consumer's copy of the
//   line and vice versa ("false sharing"). `Padded = false` puts them together so the cost
//   can be measured (bench: queue_bench).
// - Each side keeps a cached copy of the other side's index and only re-reads the shared
//   atomic when the cached value says the queue looks full (or empty). In steady state that
//   avoids a cross-core cache miss on almost every operation.
// - Memory ordering: the producer writes the slot, then publishes tail_ with release; the
//   consumer reads tail_ with acquire before reading the slot. Symmetrically for head_.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace feed {

inline constexpr std::size_t kCacheLine = 64;

template <class T, bool Padded = true>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>, "slots are copied with plain assignment");

public:
    explicit SpscQueue(std::size_t capacity_pow2) : mask_(capacity_pow2 - 1), slots_(new T[capacity_pow2]) {
        if (capacity_pow2 < 2 || (capacity_pow2 & mask_) != 0) throw std::invalid_argument("capacity must be 2^k");
    }

    std::size_t capacity() const { return mask_ + 1; }

    // Producer side. Returns false if full.
    bool try_push(const T& v) {
        const std::size_t t = p_.tail.load(std::memory_order_relaxed);
        if (t - p_.head_cache > mask_) {  // looks full: refresh our view of the consumer
            p_.head_cache = c_.head.load(std::memory_order_acquire);
            if (t - p_.head_cache > mask_) return false;
        }
        slots_[t & mask_] = v;
        p_.tail.store(t + 1, std::memory_order_release);
        return true;
    }

    // Producer side: reserve the next slot to fill in place (avoids a copy for large slots).
    // Call publish() after writing it. Returns nullptr if full.
    T* try_reserve() {
        const std::size_t t = p_.tail.load(std::memory_order_relaxed);
        if (t - p_.head_cache > mask_) {
            p_.head_cache = c_.head.load(std::memory_order_acquire);
            if (t - p_.head_cache > mask_) return nullptr;
        }
        return &slots_[t & mask_];
    }
    void publish() { p_.tail.store(p_.tail.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    // Consumer side. Returns nullptr if empty; call pop() after using the slot.
    const T* front() {
        const std::size_t h = c_.head.load(std::memory_order_relaxed);
        if (h == c_.tail_cache) {  // looks empty: refresh our view of the producer
            c_.tail_cache = p_.tail.load(std::memory_order_acquire);
            if (h == c_.tail_cache) return nullptr;
        }
        return &slots_[h & mask_];
    }
    void pop() { c_.head.store(c_.head.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    bool try_pop(T& out) {
        const T* f = front();
        if (!f) return false;
        out = *f;
        pop();
        return true;
    }

private:
    struct alignas(Padded ? kCacheLine : alignof(std::size_t)) ProducerSide {
        std::atomic<std::size_t> tail{0};
        std::size_t head_cache = 0;
    };
    struct alignas(Padded ? kCacheLine : alignof(std::size_t)) ConsumerSide {
        std::atomic<std::size_t> head{0};
        std::size_t tail_cache = 0;
    };

    const std::size_t mask_;
    std::unique_ptr<T[]> slots_;
    ProducerSide p_;
    ConsumerSide c_;
};

}  // namespace feed

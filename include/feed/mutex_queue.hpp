// Baseline for the benchmarks: the same bounded ring, guarded by a std::mutex.
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>

namespace feed {

template <class T>
class MutexQueue {
public:
    explicit MutexQueue(std::size_t capacity) : cap_(capacity), slots_(new T[capacity]) {}

    bool try_push(const T& v) {
        std::lock_guard lock(m_);
        if (tail_ - head_ == cap_) return false;
        slots_[tail_++ % cap_] = v;
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard lock(m_);
        if (tail_ == head_) return false;
        out = slots_[head_++ % cap_];
        return true;
    }

private:
    std::mutex m_;
    const std::size_t cap_;
    std::unique_ptr<T[]> slots_;
    std::size_t head_ = 0, tail_ = 0;
};

}  // namespace feed

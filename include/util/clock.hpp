// Timing and thread placement helpers.
#pragma once

#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <cstdint>

namespace util {

// Monotonic nanoseconds. steady_clock is CLOCK_MONOTONIC via the vDSO on Linux: ~20 ns, no syscall.
inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Pins the calling thread to one CPU so the scheduler cannot migrate it (which would cost
// a cold cache) and so producer and consumer run truly in parallel. Returns false on failure.
inline bool pin_to_cpu(int cpu) {
    if (cpu < 0) return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

#if defined(__x86_64__) || defined(__i386__)
inline void cpu_relax() { __builtin_ia32_pause(); }
#else
inline void cpu_relax() {}
#endif

}  // namespace util

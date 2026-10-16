// Compares the hand-off between two threads: lock-free SPSC ring (cache-line padded and not)
// against a mutex-guarded ring, for throughput and for one-way latency (half a round trip).
//   queue_bench [--items 20000000] [--trips 1000000] [--pin]
#include <cstdio>
#include <iostream>
#include <string>
#include <memory>
#include <stdexcept>
#include <thread>

#include "args.hpp"
#include "feed/mutex_queue.hpp"
#include "feed/spsc_queue.hpp"
#include "util/clock.hpp"
#include "util/histogram.hpp"

namespace {

int cpu_a = -1, cpu_b = -1;
constexpr std::size_t kCap = 1 << 14;

template <class Q>
double throughput(Q& q, uint64_t items) {
    uint64_t sum = 0;
    const int64_t t0 = util::now_ns();
    std::thread consumer([&] {
        util::pin_to_cpu(cpu_b);
        uint64_t v, got = 0;
        while (got < items)
            if (q.try_pop(v)) {
                sum += v;
                ++got;
            } else util::cpu_relax();
    });
    util::pin_to_cpu(cpu_a);
    for (uint64_t i = 0; i < items; ++i)
        while (!q.try_push(i)) util::cpu_relax();
    consumer.join();
    const double sec = double(util::now_ns() - t0) / 1e9;
    if (sum != items * (items - 1) / 2) throw std::runtime_error("lost or corrupted items");
    return double(items) / sec;
}

// Ping-pong: A sends a timestamp, B echoes it back, A records half the round trip.
template <class Q>
util::Histogram one_way(Q& ab, Q& ba, uint64_t trips) {
    util::Histogram h;
    std::thread echo([&] {
        util::pin_to_cpu(cpu_b);
        uint64_t v;
        for (uint64_t i = 0; i < trips; ++i) {
            while (!ab.try_pop(v)) util::cpu_relax();
            while (!ba.try_push(v)) util::cpu_relax();
        }
    });
    util::pin_to_cpu(cpu_a);
    uint64_t v;
    for (uint64_t i = 0; i < trips; ++i) {
        const int64_t t = util::now_ns();
        while (!ab.try_push(uint64_t(t))) util::cpu_relax();
        while (!ba.try_pop(v)) util::cpu_relax();
        h.record((util::now_ns() - int64_t(v)) / 2);
    }
    echo.join();
    return h;
}

template <class Q, class Make>
void run(const char* name, Make make, uint64_t items, uint64_t trips, bool last) {
    auto q = make();
    const double tp = throughput(*q, items);
    auto ab = make(), ba = make();
    const auto h = one_way(*ab, *ba, trips);
    std::printf("  \"%s\": {\"items_per_sec\": %.0f, \"one_way_ns\": %s}%s\n", name, tp, h.summary_json().c_str(),
                last ? "" : ",");
}

}  // namespace

int main(int argc, char** argv) try {
    const Args a(argc, argv);
    const uint64_t items = uint64_t(a.num("items", 20e6)), trips = uint64_t(a.num("trips", 1e6));
    if (a.has("pin")) {
        cpu_a = int(a.num("cpu-a", 1));
        cpu_b = int(a.num("cpu-b", 2));
    }
    std::printf("{\"pinned\": %s, \"items\": %llu, \"trips\": %llu,\n", a.has("pin") ? "true" : "false",
                (unsigned long long)items, (unsigned long long)trips);
    run<feed::SpscQueue<uint64_t, true>>("spsc_padded", [] { return std::make_unique<feed::SpscQueue<uint64_t, true>>(kCap); },
                                         items, trips, false);
    run<feed::SpscQueue<uint64_t, false>>("spsc_unpadded", [] { return std::make_unique<feed::SpscQueue<uint64_t, false>>(kCap); },
                                          items, trips, false);
    run<feed::MutexQueue<uint64_t>>("mutex", [] { return std::make_unique<feed::MutexQueue<uint64_t>>(kCap); },
                                    items / 4, trips / 4, true);
    std::printf("}\n");
} catch (const std::exception& e) {
    std::cerr << "queue_bench: " << e.what() << "\n";
    return 1;
}

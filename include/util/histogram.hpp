// Latency histogram with fixed relative precision (log-linear buckets, like HdrHistogram).
//
// Each power of two is split into 2^kSubBits linear buckets, so any recorded value is
// reported within 1/2^kSubBits (about 0.8%) of its true value, from 1 ns to hours, in a
// fixed 6,400-slot array. Recording is a few integer instructions and never allocates, so
// it can sit on the hot path.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace util {

class Histogram {
public:
    static constexpr int kSubBits = 7;
    static constexpr int kSub = 1 << kSubBits;
    static constexpr int kBuckets = 50 * kSub;

    void record(int64_t v) {
        if (v < 0) v = 0;
        ++counts_[index(uint64_t(v))];
        ++n_;
        max_ = std::max(max_, v);
        min_ = std::min(min_, v);
        sum_ += double(v);
    }

    uint64_t count() const { return n_; }
    int64_t max() const { return n_ ? max_ : 0; }
    int64_t min() const { return n_ ? min_ : 0; }
    double mean() const { return n_ ? sum_ / double(n_) : 0.0; }

    // Value at quantile q in [0, 1] (upper edge of the bucket holding it, capped at max).
    int64_t percentile(double q) const {
        if (n_ == 0) return 0;
        const uint64_t rank = std::max<uint64_t>(1, uint64_t(q * double(n_) + 0.5));
        uint64_t seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) return std::min<int64_t>(int64_t(upper(i)), max_);
        }
        return max_;
    }

    void merge(const Histogram& o) {
        for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        n_ += o.n_;
        sum_ += o.sum_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
    }

    std::string summary_json() const {
        auto f = [](int64_t v) { return std::to_string(v); };
        return "{\"count\": " + std::to_string(n_) + ", \"min\": " + f(min()) + ", \"p50\": " + f(percentile(0.5)) +
               ", \"p90\": " + f(percentile(0.9)) + ", \"p99\": " + f(percentile(0.99)) +
               ", \"p99_9\": " + f(percentile(0.999)) + ", \"p99_99\": " + f(percentile(0.9999)) +
               ", \"max\": " + f(max()) + ", \"mean\": " + std::to_string(mean()) + "}";
    }

    static int index(uint64_t v) {
        if (v < kSub) return int(v);
        const int msb = 63 - __builtin_clzll(v);           // v in [2^msb, 2^(msb+1))
        const int shift = msb - kSubBits;                  // keep kSubBits bits below the msb
        return (shift + 1) * kSub + int((v >> shift) - kSub);
    }
    static uint64_t upper(int i) {  // largest value mapping to bucket i
        if (i < kSub) return uint64_t(i);
        const int shift = i / kSub - 1;
        const uint64_t sub = uint64_t(i % kSub) + kSub;
        return ((sub + 1) << shift) - 1;
    }

private:
    std::array<uint64_t, kBuckets> counts_{};
    uint64_t n_ = 0;
    int64_t max_ = 0;
    int64_t min_ = INT64_MAX;
    double sum_ = 0;
};

}  // namespace util

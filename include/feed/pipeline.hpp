// The live path: MoldUDP64 packets over UDP multicast into the order book.
//
//   sender thread ──UDP multicast──▶ receive thread ──SPSC ring──▶ book thread
//                                    (recv into slot)              (gap check, parse, apply)
//
// The receive thread does nothing but drain the socket into ring slots, so the kernel's
// socket buffer never fills while the book thread is busy with a burst. Each thread can be
// pinned to its own core. Every packet carries four timestamps, giving latency histograms for
// each hop: send -> receive (kernel + polling), receive -> dequeue (time in the ring),
// dequeue -> applied (parse + book update), and send -> applied (end to end).
#pragma once

#include <cstdint>
#include <string>

#include "book/order_book.hpp"
#include "itch/framing.hpp"
#include "util/histogram.hpp"

namespace feed {

struct PipelineConfig {
    std::string group = "239.1.1.1";
    uint16_t port = 30001;
    std::string iface = "127.0.0.1";
    double rate = 1e6;               // messages per second the sender paces to; 0 = as fast as possible
    int send_cpu = -1, rx_cpu = -1, book_cpu = -1;  // -1 = not pinned
    std::size_t queue_slots = 4096;  // ring capacity in packets (power of two)
    int rcvbuf = 8 << 20;
    uint64_t drop_every = 0;         // sender skips every Nth packet (tests gap detection); 0 = none
    int64_t idle_timeout_ns = 100'000'000;  // receiver stops this long after the last packet
    bool prefetch = true;            // prefetch a packet's orders before applying it
};

struct PipelineResult {
    uint64_t packets_sent = 0, packets_skipped = 0, send_errors = 0;
    uint64_t packets_received = 0, bad_packets = 0, messages_applied = 0;
    uint64_t gaps = 0, lost_messages = 0;
    uint64_t ring_full_spins = 0;    // times the receive thread found the ring full
    int rcvbuf_granted = 0;
    double seconds = 0;              // first send to last packet applied
    util::Histogram wire, ring, apply, end_to_end;  // nanoseconds per packet
};

PipelineResult run_pipeline(const itch::Packetized& packets, const PipelineConfig& cfg, book::OrderBook& book);

}  // namespace feed

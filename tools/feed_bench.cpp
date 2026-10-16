// End-to-end live-path benchmark: synthetic ITCH -> MoldUDP64 packets -> UDP multicast on
// loopback -> receive thread -> SPSC ring -> book thread. Prints per-hop latency percentiles.
//   feed_bench [--messages 5000000] [--rate 1000000] [--pin] [--no-prefetch] [--drop-every N] [--slots 4096]
// --rate 0 sends as fast as the sender can, which shows where packets start being lost.
#include <iostream>
#include <memory>

#include "args.hpp"
#include "feed/pipeline.hpp"
#include "itch/generator.hpp"

int main(int argc, char** argv) try {
    const Args a(argc, argv);
    itch::GenConfig g;
    g.messages = uint64_t(a.num("messages", 5e6));
    g.live_target = uint64_t(a.num("live", 1e6));
    g.symbols = uint32_t(a.num("symbols", 3000));
    const auto framed = itch::generate_framed(g);
    const auto packets = itch::packetize(framed, "BENCH00001");

    feed::PipelineConfig cfg;
    cfg.rate = a.num("rate", 1e6);
    cfg.queue_slots = std::size_t(a.num("slots", 4096));
    cfg.drop_every = uint64_t(a.num("drop-every", 0));
    cfg.port = uint16_t(a.num("port", 30001));
    cfg.prefetch = !a.has("no-prefetch");
    if (a.has("pin")) {  // sender, receiver and book on separate cores
        cfg.send_cpu = int(a.num("send-cpu", 0));
        cfg.rx_cpu = int(a.num("rx-cpu", 1));
        cfg.book_cpu = int(a.num("book-cpu", 2));
    }
    auto book = std::make_unique<book::OrderBook>(1 << 21);
    const auto r = feed::run_pipeline(packets, cfg, *book);

    std::cout << "{\"messages\": " << packets.messages << ", \"packets\": " << packets.offset.size()
              << ", \"rate_target\": " << cfg.rate << ", \"pinned\": " << (a.has("pin") ? "true" : "false")
              << ", \"prefetch\": " << (cfg.prefetch ? "true" : "false")
              << ", \"seconds\": " << r.seconds << ", \"messages_per_sec\": " << double(r.messages_applied) / r.seconds
              << ", \"packets_sent\": " << r.packets_sent << ", \"packets_skipped\": " << r.packets_skipped
              << ", \"send_errors\": " << r.send_errors << ", \"packets_received\": " << r.packets_received
              << ", \"bad_packets\": " << r.bad_packets << ", \"messages_applied\": " << r.messages_applied
              << ", \"gaps\": " << r.gaps << ", \"lost_messages\": " << r.lost_messages
              << ", \"ring_full_spins\": " << r.ring_full_spins << ", \"rcvbuf_bytes\": " << r.rcvbuf_granted
              << ", \"unknown_ref\": " << book->stats().unknown_ref << ", \"live_orders_end\": " << book->live_orders()
              << ",\n \"ns_send_to_receive\": " << r.wire.summary_json()
              << ",\n \"ns_in_ring\": " << r.ring.summary_json()
              << ",\n \"ns_parse_and_apply_per_packet\": " << r.apply.summary_json()
              << ",\n \"ns_end_to_end\": " << r.end_to_end.summary_json() << "}\n";
} catch (const std::exception& e) {
    std::cerr << "feed_bench: " << e.what() << "\n";
    return 1;
}

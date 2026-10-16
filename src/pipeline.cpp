#include "feed/pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

#include "feed/spsc_queue.hpp"
#include "feed/udp.hpp"
#include "util/clock.hpp"

namespace feed {
namespace {

struct PacketSlot {
    int64_t rx_ns;
    uint32_t len;
    std::byte data[itch::kMaxPacket];
};

}  // namespace

PipelineResult run_pipeline(const itch::Packetized& pk, const PipelineConfig& cfg, book::OrderBook& book) {
    PipelineResult res;
    const std::size_t n = pk.offset.size();
    // Send times are written by the sender and read by the book thread after the packet has
    // crossed the kernel. Relaxed atomics make that hand-off explicit to ThreadSanitizer.
    auto sent_ns = std::make_unique<std::atomic<int64_t>[]>(n);
    SpscQueue<PacketSlot> ring(cfg.queue_slots);
    UdpReceiver rx(cfg.group, cfg.port, cfg.iface, cfg.rcvbuf);
    UdpSender tx(cfg.group, cfg.port, cfg.iface);
    res.rcvbuf_granted = rx.rcvbuf_bytes();

    std::atomic<bool> sender_done{false}, rx_done{false};
    std::atomic<int64_t> start_ns{0};
    int64_t end_ns = 0;

    std::thread book_thread([&] {
        util::pin_to_cpu(cfg.book_cpu);
        itch::GapDetector gaps;
        for (;;) {
            const PacketSlot* s = ring.front();
            if (!s) {
                if (rx_done.load(std::memory_order_acquire) && !ring.front()) break;
                util::cpu_relax();
                continue;
            }
            const int64_t t_deq = util::now_ns();
            ++res.packets_received;
            const std::span<const std::byte> pkt(s->data, s->len);
            if (pkt.size() < itch::kMoldHeader) {
                ++res.bad_packets;
                ring.pop();
                continue;
            }
            const itch::MoldHeader h = itch::decode_mold_header(pkt.data());
            gaps.on_packet(h.sequence, h.count);
            // Two passes over the packet: the first starts the cache misses for every order the
            // packet touches, the second applies the messages while those loads are in flight.
            if (cfg.prefetch)
                itch::for_each_in_packet(pkt, [&](const std::byte* m, std::size_t) { book.prefetch(m); });
            uint64_t applied = 0;
            const auto err = itch::for_each_in_packet(pkt, [&](const std::byte* m, std::size_t) {
                book.apply(m);
                ++applied;
            });
            const int64_t t_done = util::now_ns();
            res.messages_applied += applied;
            if (err != itch::PacketError::None) ++res.bad_packets;

            const auto it = std::upper_bound(pk.first_seq.begin(), pk.first_seq.end(), h.sequence);
            if (it != pk.first_seq.begin() && *(it - 1) == h.sequence) {
                const int64_t t_sent = sent_ns[std::size_t(it - pk.first_seq.begin()) - 1].load(std::memory_order_relaxed);
                res.wire.record(s->rx_ns - t_sent);
                res.end_to_end.record(t_done - t_sent);
            }
            res.ring.record(t_deq - s->rx_ns);
            res.apply.record(t_done - t_deq);
            end_ns = t_done;
            ring.pop();
        }
        res.gaps = gaps.gaps();
        res.lost_messages = gaps.lost_messages();
    });

    std::thread rx_thread([&] {
        util::pin_to_cpu(cfg.rx_cpu);
        int64_t last = 0;
        for (;;) {
            PacketSlot* slot = ring.try_reserve();
            if (!slot) {
                ++res.ring_full_spins;
                util::cpu_relax();
                continue;
            }
            const long got = rx.try_recv(slot->data, sizeof(slot->data));
            const int64_t now = util::now_ns();
            if (got > 0) {
                slot->rx_ns = now;
                slot->len = uint32_t(got);
                ring.publish();
                last = now;
                continue;
            }
            if (sender_done.load(std::memory_order_acquire)) {
                if (last == 0) last = now;
                if (now - last > cfg.idle_timeout_ns) break;
            }
            util::cpu_relax();
        }
        rx_done.store(true, std::memory_order_release);
    });

    std::thread send_thread([&] {
        util::pin_to_cpu(cfg.send_cpu);
        const int64_t t0 = util::now_ns();
        start_ns.store(t0, std::memory_order_relaxed);
        const double ns_per_msg = cfg.rate > 0 ? 1e9 / cfg.rate : 0;
        uint64_t msgs = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t count = pk.first_seq.size() > i + 1 ? pk.first_seq[i + 1] - pk.first_seq[i]
                                                                  : pk.messages + pk.first_seq[0] - pk.first_seq[i];
            if (ns_per_msg > 0) {  // pace on the message count: due time of this packet's first message
                const int64_t due = t0 + int64_t(double(msgs) * ns_per_msg);
                while (util::now_ns() < due) util::cpu_relax();
            }
            msgs += count;
            if (cfg.drop_every && (i + 1) % cfg.drop_every == 0) {
                ++res.packets_skipped;
                continue;
            }
            sent_ns[i].store(util::now_ns(), std::memory_order_relaxed);
            if (tx.send(pk.bytes.data() + pk.offset[i], pk.length[i])) ++res.packets_sent;
            else ++res.send_errors;
        }
        sender_done.store(true, std::memory_order_release);
    });

    send_thread.join();
    rx_thread.join();
    book_thread.join();
    res.seconds = double(end_ns - start_ns.load()) / 1e9;
    return res;
}

}  // namespace feed

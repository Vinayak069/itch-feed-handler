#include "itch/framing.hpp"

#include <algorithm>
#include <stdexcept>

namespace itch {

Packetized packetize(std::span<const std::byte> framed, const std::string& session, uint64_t first_seq) {
    Packetized out;
    out.bytes.reserve(framed.size() + framed.size() / 50 + kMaxPacket);
    std::array<char, 10> sess;
    sess.fill(' ');
    std::copy_n(session.data(), std::min<std::size_t>(10, session.size()), sess.data());

    uint64_t seq = first_seq;
    std::size_t pkt_start = 0;
    uint16_t count = 0;
    auto open_packet = [&] {
        pkt_start = out.bytes.size();
        out.bytes.resize(pkt_start + kMoldHeader);
        std::memcpy(out.bytes.data() + pkt_start, sess.data(), 10);
        put64(out.bytes.data() + pkt_start + 10, seq);
        count = 0;
    };
    auto close_packet = [&] {
        put16(out.bytes.data() + pkt_start + 18, count);
        out.offset.push_back(uint32_t(pkt_start));
        out.length.push_back(uint16_t(out.bytes.size() - pkt_start));
        out.first_seq.push_back(seq - count);
    };

    open_packet();
    const FrameResult r = for_each_framed(framed, [&](const std::byte* msg, std::size_t len) {
        if (out.bytes.size() - pkt_start + 2 + len > kMaxPacket) {
            close_packet();
            open_packet();
        }
        const std::size_t at = out.bytes.size();
        out.bytes.resize(at + 2 + len);
        put16(out.bytes.data() + at, uint16_t(len));
        std::memcpy(out.bytes.data() + at + 2, msg, len);
        ++count;
        ++seq;
        ++out.messages;
    });
    if (r.error != FrameError::None || r.consumed != framed.size())
        throw std::runtime_error("packetize: input is not a clean sequence of ITCH messages");
    if (count > 0) close_packet();
    else out.bytes.resize(pkt_start);
    return out;
}

}  // namespace itch

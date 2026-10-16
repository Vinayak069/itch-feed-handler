// Two framings of the same ITCH messages:
//
// 1. Files (NASDAQ's historical captures): each message is preceded by a 2-byte big-endian length.
// 2. MoldUDP64, NASDAQ's live multicast transport: a 20-byte packet header (10-byte session,
//    8-byte sequence number of the first message, 2-byte message count) followed by
//    length-prefixed messages. Sequence numbers let a receiver detect lost packets.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "itch/messages.hpp"

namespace itch {

enum class FrameError { None, UnknownType, BadLength, Truncated };

struct FrameResult {
    std::size_t consumed = 0;  // bytes of complete, valid messages
    std::size_t messages = 0;
    FrameError error = FrameError::None;
};

// Calls on_msg(const std::byte* msg, std::size_t len) for each complete message in `buf`.
// Stops at the first malformed message (unknown type, or a length that disagrees with the
// spec for that type). A message cut off at the end of `buf` is left unconsumed, so a
// chunked reader can carry it over to the next read.
template <class F>
FrameResult for_each_framed(std::span<const std::byte> buf, F&& on_msg) {
    FrameResult r;
    const std::byte* p = buf.data();
    const std::byte* end = p + buf.size();
    while (end - p >= 2) {
        const uint16_t len = be16(p);
        if (end - p - 2 < len) break;  // incomplete: carry over
        if (len == 0) { r.error = FrameError::BadLength; return r; }
        const std::size_t want = expected_size(p[2]);
        if (want == 0) { r.error = FrameError::UnknownType; return r; }
        if (want != len) { r.error = FrameError::BadLength; return r; }
        on_msg(p + 2, std::size_t(len));
        p += 2 + len;
        ++r.messages;
    }
    r.consumed = std::size_t(p - buf.data());
    return r;
}

// ---- MoldUDP64 -----------------------------------------------------------------------------
constexpr std::size_t kMoldHeader = 20;
constexpr std::size_t kMaxPacket = 1400;  // stay under a 1500-byte Ethernet MTU with IP/UDP headers

struct MoldHeader {
    std::array<char, 10> session;
    uint64_t sequence;  // sequence number of the first message in the packet
    uint16_t count;
};

inline MoldHeader decode_mold_header(const std::byte* p) {
    MoldHeader h;
    std::memcpy(h.session.data(), p, 10);
    h.sequence = be64(p + 10);
    h.count = be16(p + 18);
    return h;
}

// Packs length-prefixed messages (file framing) into MoldUDP64 packets of at most kMaxPacket
// bytes. Sequence numbers start at `first_seq`.
struct Packetized {
    std::vector<std::byte> bytes;       // all packets back to back
    std::vector<uint32_t> offset;       // start of each packet in `bytes`
    std::vector<uint16_t> length;       // size of each packet
    std::vector<uint64_t> first_seq;    // sequence number of each packet's first message
    uint64_t messages = 0;
};
Packetized packetize(std::span<const std::byte> framed, const std::string& session, uint64_t first_seq = 1);

enum class PacketError { None, TooShort, BadMessage, CountMismatch };

// Calls on_msg for each message in a MoldUDP64 packet; validates lengths against the spec.
template <class F>
PacketError for_each_in_packet(std::span<const std::byte> pkt, F&& on_msg) {
    if (pkt.size() < kMoldHeader) return PacketError::TooShort;
    const MoldHeader h = decode_mold_header(pkt.data());
    const FrameResult r = for_each_framed(pkt.subspan(kMoldHeader), on_msg);
    if (r.error != FrameError::None || r.consumed != pkt.size() - kMoldHeader) return PacketError::BadMessage;
    if (r.messages != h.count) return PacketError::CountMismatch;
    return PacketError::None;
}

// Tracks MoldUDP64 sequence numbers: counts packets that arrive after a gap and messages lost.
class GapDetector {
public:
    // Returns the number of messages missing before this packet (0 if in order).
    uint64_t on_packet(uint64_t first_seq, uint16_t count) {
        uint64_t missing = 0;
        if (next_ != 0 && first_seq > next_) {
            missing = first_seq - next_;
            lost_ += missing;
            ++gaps_;
        }
        if (first_seq + count > next_) next_ = first_seq + count;  // duplicates and late packets are ignored
        return missing;
    }
    uint64_t gaps() const { return gaps_; }
    uint64_t lost_messages() const { return lost_; }
    uint64_t next_expected() const { return next_; }

private:
    uint64_t next_ = 0, gaps_ = 0, lost_ = 0;
};

}  // namespace itch

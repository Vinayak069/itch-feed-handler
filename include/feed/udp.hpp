// Thin RAII wrappers over Linux UDP multicast sockets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace feed {

class UdpSender {
public:
    // Sends to group:port through the interface with address `iface` (loopback by default).
    UdpSender(const std::string& group, uint16_t port, const std::string& iface = "127.0.0.1");
    ~UdpSender();
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    bool send(const void* data, std::size_t len);

private:
    int fd_ = -1;
    unsigned char addr_[16];  // sockaddr_in, kept opaque to keep <netinet/in.h> out of the header
};

class UdpReceiver {
public:
    // Joins group on `iface` and binds port. `rcvbuf` asks the kernel for that many bytes of
    // socket buffer: the slack that absorbs bursts while the receive thread is busy.
    UdpReceiver(const std::string& group, uint16_t port, const std::string& iface = "127.0.0.1",
                int rcvbuf = 8 << 20);
    ~UdpReceiver();
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // Non-blocking receive: returns bytes read, 0 if nothing is waiting, -1 on error.
    long try_recv(void* buf, std::size_t cap);
    int rcvbuf_bytes() const { return rcvbuf_; }  // what the kernel actually granted

private:
    int fd_ = -1;
    int rcvbuf_ = 0;
};

}  // namespace feed

#include "feed/udp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace feed {
namespace {

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string(what) + ": " + std::strerror(errno)); }

in_addr parse(const std::string& ip) {
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) throw std::invalid_argument("bad IPv4 address: " + ip);
    return a;
}

}  // namespace

UdpSender::UdpSender(const std::string& group, uint16_t port, const std::string& iface) {
    static_assert(sizeof(addr_) >= sizeof(sockaddr_in));
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) fail("socket");
    const in_addr ifa = parse(iface);
    if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa)) < 0) fail("IP_MULTICAST_IF");
    const unsigned char loop = 1, ttl = 1;
    if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) fail("IP_MULTICAST_LOOP");
    if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) fail("IP_MULTICAST_TTL");
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr = parse(group);
    std::memcpy(addr_, &to, sizeof(to));
}

UdpSender::~UdpSender() {
    if (fd_ >= 0) ::close(fd_);
}

bool UdpSender::send(const void* data, std::size_t len) {
    return ::sendto(fd_, data, len, 0, reinterpret_cast<const sockaddr*>(addr_), sizeof(sockaddr_in)) ==
           ssize_t(len);
}

UdpReceiver::UdpReceiver(const std::string& group, uint16_t port, const std::string& iface, int rcvbuf) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) fail("socket");
    const int one = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) fail("SO_REUSEADDR");
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) fail("SO_RCVBUF");
    socklen_t sl = sizeof(rcvbuf_);
    ::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf_, &sl);  // capped by net.core.rmem_max

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr = parse(group);  // bind to the group so unrelated traffic on the port is filtered
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) fail("bind");

    ip_mreq mreq{};
    mreq.imr_multiaddr = parse(group);
    mreq.imr_interface = parse(iface);
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) fail("IP_ADD_MEMBERSHIP");
}

UdpReceiver::~UdpReceiver() {
    if (fd_ >= 0) ::close(fd_);
}

long UdpReceiver::try_recv(void* buf, std::size_t cap) {
    const ssize_t n = ::recv(fd_, buf, cap, MSG_DONTWAIT);
    if (n >= 0) return long(n);
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

}  // namespace feed

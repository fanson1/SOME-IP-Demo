#ifndef SOMEIP_LEGACY_NET_HPP
#define SOMEIP_LEGACY_NET_HPP

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <unistd.h>
#include <cstring>
#include <string>

#include "sd.hpp"  // someip::legacy::SD_PORT / SD_MULTICAST_ADDRESS

namespace someip { namespace legacy {

inline bool calc_ip(const struct sockaddr_in &addr, std::string &out) {
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    out = buf;
    return true;
}

inline sockaddr_in make_addr(const std::string &ip, uint16_t port) {
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    return addr;
}

inline int send_to(int fd, const sockaddr_in &to, const uint8_t *data, size_t size) {
    return (int)sendto(fd, data, size, 0,
                       (const sockaddr *)&to, sizeof(to));
}

inline int make_udp_socket(uint16_t port, bool reuse_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (reuse_port) {
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    }
    sockaddr_in bind_addr;
    std::memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (const sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

inline int local_ipv4(std::string &out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    sockaddr_in target = make_addr("8.8.8.8", 80);
    int rc = connect(fd, (const sockaddr *)&target, sizeof(target));
    if (rc == 0) {
        sockaddr_in self;
        socklen_t len = sizeof(self);
        if (getsockname(fd, (sockaddr *)&self, &len) == 0) {
            calc_ip(self, out);
        }
    }
    ::close(fd);
    return rc;
}

inline int make_sd_socket(const std::string &interface_ip) {
    int fd = make_udp_socket(someip::legacy::SD_PORT, true);
    if (fd < 0) return -1;
    ip_mreq mreq;
    std::memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, someip::legacy::SD_MULTICAST_ADDRESS, &mreq.imr_multiaddr);
    inet_pton(AF_INET, interface_ip.c_str(), &mreq.imr_interface);
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    struct in_addr ifaddr;
    inet_pton(AF_INET, interface_ip.c_str(), &ifaddr);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
    int one = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
    unsigned char ttl = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    return fd;
}

} // namespace legacy
} // namespace someip

#endif // SOMEIP_LEGACY_NET_HPP
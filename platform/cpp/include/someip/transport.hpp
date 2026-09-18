#ifndef SOMEIP_TRANSPORT_HPP
#define SOMEIP_TRANSPORT_HPP

// Transport layer: UDP (unicast/multicast) and TCP endpoints.
// Mirrors platform/python/someip/transport.py (C++17, POSIX, no third-party deps).
//
// - UdpEndpoint: bound datagram socket, thread-safe send() with a mutex,
//   poll()/recvfrom() receive with a timeout.
// - TcpConnection: buffered byte stream that extracts complete SOME/IP
//   frames (handles split / coalesced segments), maximum frame cap.
// - TcpListener: accept loop with timeout.

#include "wire.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#ifndef SOMEIP_MAX_TCP_FRAME
#define SOMEIP_MAX_TCP_FRAME (16 * 1024 * 1024)  // 16 MiB cap per SOME/IP frame
#endif
#define SOMEIP_POLL_MS 500

// macOS has no MSG_NOSIGNAL; use SO_NOSIGPIPE to avoid SIGPIPE crashes.
#ifdef MSG_NOSIGNAL
#define SOMEIP_SEND_FLAGS MSG_NOSIGNAL
#else
#define SOMEIP_SEND_FLAGS 0
#endif

namespace someip {

struct TransportError : std::runtime_error {
    explicit TransportError(const std::string &what) : std::runtime_error(what) {}
};
struct ReceiveTimeout : TransportError {
    ReceiveTimeout() : TransportError("receive timeout") {}
};
struct FrameTooLarge : TransportError {
    explicit FrameTooLarge(size_t n)
        : TransportError("frame exceeds " + std::to_string(n) + " byte cap") {}
};

namespace detail {
inline void close_fd(int &fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}
inline void set_nosigpipe(int fd) {
#ifndef MSG_NOSIGNAL
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}
inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace detail

class UdpEndpoint {
public:
    UdpEndpoint() : fd_(-1), send_lock_(new std::mutex) {}
    UdpEndpoint(const UdpEndpoint &) = delete;
    UdpEndpoint &operator=(const UdpEndpoint &) = delete;
    UdpEndpoint(UdpEndpoint &&o) noexcept
        : fd_(o.fd_), send_lock_(std::move(o.send_lock_)) {
        o.fd_ = -1;
    }
    UdpEndpoint &operator=(UdpEndpoint &&o) noexcept {
        if (this != &o) {
            close();
            fd_ = o.fd_;
            send_lock_ = std::move(o.send_lock_);
            o.fd_ = -1;
        }
        return *this;
    }
    ~UdpEndpoint() { close(); }

    // Bound unicast datagram socket on (address, port). port 0 = ephemeral.
    static UdpEndpoint unicast(uint16_t port = 0, const char *address = "0.0.0.0",
                               bool reuse = false) {
        int fd = make_socket(reuse);
        sockaddr_in addr = ipv4(address, port);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            detail::close_fd(fd);
            throw TransportError("udp bind " + std::string(address) + ":" +
                                 std::to_string(port) + " failed");
        }
        UdpEndpoint e;
        e.fd_ = fd;
        return e;
    }

    // Receiver socket bound to a multicast group on `port`.
    static UdpEndpoint multicast(uint16_t port, const char *group,
                                 const char *interface_ip = nullptr,
                                 bool reuse = true) {
        int fd = make_socket(reuse);
        sockaddr_in wild = ipv4("0.0.0.0", port);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&wild), sizeof(wild)) != 0) {
            detail::close_fd(fd);
            throw TransportError("multicast bind failed on port " + std::to_string(port));
        }
        UdpEndpoint e;
        e.fd_ = fd;
        e.join_group(group, interface_ip);
        return e;
    }

    // Sender socket for emitting traffic to a multicast group.
    static UdpEndpoint multicast_sender(const char *group,
                                        const char *interface_ip = nullptr,
                                        uint8_t ttl = 4) {
        int fd = make_socket(false);
        sockaddr_in wild = ipv4("0.0.0.0", 0);
        (void)::bind(fd, reinterpret_cast<sockaddr *>(&wild), sizeof(wild));
        uint8_t t = ttl;
        ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof(t));
        UdpEndpoint e;
        e.fd_ = fd;
        e.join_group(group, interface_ip);
        return e;
    }

    void join_group(const char *group, const char *interface_ip = nullptr) {
        ip_mreq mreq;
        std::memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr.s_addr = ::inet_addr(group);
        mreq.imr_interface.s_addr = interface_ip != nullptr
                                        ? ::inet_addr(interface_ip)
                                        : htonl(INADDR_ANY);
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            throw TransportError("cannot join multicast group " + std::string(group));
        }
    }

    // Thread-safe datagram send; throws TransportError on failure or timeout.
    ssize_t send(const uint8_t *data, size_t n, const sockaddr_in &dst) {
        std::lock_guard<std::mutex> lock(*send_lock_);
        ssize_t r = ::sendto(fd_, data, n, 0, reinterpret_cast<const sockaddr *>(&dst),
                             sizeof(dst));
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                pollfd p{fd_, POLLOUT, 0};
                if (::poll(&p, 1, SOMEIP_POLL_MS) != 1) {
                    throw TransportError("send buffer full / endpoint closed");
                }
                r = ::sendto(fd_, data, n, 0,
                             reinterpret_cast<const sockaddr *>(&dst), sizeof(dst));
            }
            if (r < 0) {
                throw TransportError("udp send failed: " + std::string(std::strerror(errno)));
            }
        }
        return r;
    }

    // Block up to timeout_ms for a datagram. Returns false on timeout.
    bool recv(std::vector<uint8_t> &out, sockaddr_in &src, int timeout_ms) {
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) != 1) {
            return false;
        }
        socklen_t len = sizeof(src);
        out.resize(65535);
        ssize_t r = ::recvfrom(fd_, out.data(), out.size(), 0,
                               reinterpret_cast<sockaddr *>(&src), &len);
        if (r <= 0) {
            return false;
        }
        out.resize(static_cast<size_t>(r));
        return true;
    }

    uint16_t local_port() const {
        sockaddr_in a;
        socklen_t len = sizeof(a);
        if (::getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len) == 0) {
            return ntohs(a.sin_port);
        }
        return 0;
    }

    int fileno() const { return fd_; }
    void close() { detail::close_fd(fd_); }

    static sockaddr_in ipv4(const char *host, uint16_t port) {
        sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = ::inet_addr(host);
        if (a.sin_addr.s_addr == INADDR_NONE) {
            throw TransportError("invalid ipv4 address: " + std::string(host));
        }
        return a;
    }

private:
    static int make_socket(bool reuse) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            throw TransportError("socket() failed");
        }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (reuse) {
            (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
        }
        return fd;
    }

    int fd_;
    std::unique_ptr<std::mutex> send_lock_;
};

class TcpConnection {
public:
    TcpConnection() : fd_(-1), send_lock_(new std::mutex) {}
    TcpConnection(const TcpConnection &) = delete;
    TcpConnection &operator=(const TcpConnection &) = delete;
    TcpConnection(TcpConnection &&o) noexcept
        : fd_(o.fd_), buf_(std::move(o.buf_)), send_lock_(std::move(o.send_lock_)) {
        o.fd_ = -1;
    }
    TcpConnection &operator=(TcpConnection &&o) noexcept {
        if (this != &o) {
            close();
            fd_ = o.fd_;
            buf_ = std::move(o.buf_);
            send_lock_ = std::move(o.send_lock_);
            o.fd_ = -1;
        }
        return *this;
    }
    ~TcpConnection() { close(); }

    // Wrap an already-connected/accepted fd.
    static TcpConnection from_fd(int fd) {
        detail::set_nosigpipe(fd);
        TcpConnection c;
        c.fd_ = fd;
        return c;
    }

    static TcpConnection connect(const char *host, uint16_t port, int timeout_ms = 500) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            throw TransportError("socket() failed");
        }
        detail::set_nosigpipe(fd);
        sockaddr_in dst = UdpEndpoint::ipv4(host, port);
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
        if (rc != 0 && errno != EINPROGRESS) {
            detail::close_fd(fd);
            throw TransportError("connect " + std::string(host) + ":" +
                                 std::to_string(port) + " failed: " +
                                 std::strerror(errno));
        }
        if (rc != 0) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, timeout_ms) != 1) {
                detail::close_fd(fd);
                throw TransportError("connect " + std::string(host) + ":" +
                                     std::to_string(port) + " timed out");
            }
            int soerr = 0;
            socklen_t slen = sizeof(soerr);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
            if (soerr != 0) {
                detail::close_fd(fd);
                throw TransportError("connect " + std::string(host) + ":" +
                                     std::to_string(port) + " failed: " +
                                     std::strerror(soerr));
            }
        }
        return from_fd(fd);
    }

    int fileno() const { return fd_; }

    void send_frame(const Message &msg) {
        std::vector<uint8_t> raw;
        msg.to_bytes(raw);
        send_raw(raw.data(), raw.size());
    }

    // Raw (possibly split) byte write, used by tests and streaming sources.
    void send_raw(const uint8_t *data, size_t n) {
        std::lock_guard<std::mutex> lock(*send_lock_);
        size_t off = 0;
        while (off < n) {
            ssize_t sent = ::send(fd_, data + off, n - off, SOMEIP_SEND_FLAGS);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    pollfd p{fd_, POLLOUT, 0};
                    if (::poll(&p, 1, SOMEIP_POLL_MS) != 1) {
                        throw TransportError("send raw failed: socket busy");
                    }
                    continue;
                }
                throw TransportError("send raw failed: " +
                                     std::string(std::strerror(errno)));
            }
            off += static_cast<size_t>(sent);
        }
    }

    // Return next complete SOME/IP frame, or std::nullopt on clean EOF, or
    // throw ReceiveTimeout when a full frame did not arrive within timeout_ms.
    // Loops internally until a complete frame is available (frames may be
    // delivered in many small segments).
    std::optional<Message> recv_frame(int timeout_ms) {
        const int64_t deadline_ms = detail::now_ms() + timeout_ms;
        for (;;) {
            const int64_t remaining = deadline_ms - detail::now_ms();
            if (remaining <= 0) {
                throw ReceiveTimeout();
            }
            pollfd p{fd_, POLLIN, 0};
            int pr = ::poll(&p, 1, static_cast<int>(remaining));
            if (pr == 0) {
                continue;  // loop head raises ReceiveTimeout once expired
            }
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw TransportError("poll failed in recv_frame");
            }
            uint8_t chunk[65536];
            ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n == 0) {
                return std::nullopt;  // clean EOF
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                throw TransportError("recv failed: " + std::string(std::strerror(errno)));
            }
            buf_.insert(buf_.end(), chunk, chunk + n);
            if (auto frame = extract_frame()) {
                return frame;
            }
        }
    }

    void close() {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            detail::close_fd(fd_);
        }
    }

private:
    std::optional<Message> extract_frame() {
        while (buf_.size() >= 16) {
            uint32_t len = (uint32_t(buf_[4]) << 24) | (uint32_t(buf_[5]) << 16) |
                           (uint32_t(buf_[6]) << 8) | uint32_t(buf_[7]);
            const size_t frame_size = 8 + static_cast<size_t>(len);
            if (frame_size > SOMEIP_MAX_TCP_FRAME) {
                throw FrameTooLarge(SOMEIP_MAX_TCP_FRAME);
            }
            if (buf_.size() < frame_size) {
                return std::nullopt;  // need more bytes
            }
            Message m = Message::from_bytes(buf_.data(), buf_.size());
            buf_.erase(buf_.begin(), buf_.begin() + frame_size);
            return m;
        }
        return std::nullopt;
    }

    int fd_;
    std::vector<uint8_t> buf_;
    std::unique_ptr<std::mutex> send_lock_;
};

class TcpListener {
public:
    TcpListener() : fd_(-1) {}
    TcpListener(const TcpListener &) = delete;
    TcpListener &operator=(const TcpListener &) = delete;
    TcpListener(TcpListener &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    TcpListener &operator=(TcpListener &&o) noexcept {
        if (this != &o) {
            close();
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }
    ~TcpListener() { close(); }

    static TcpListener bind(uint16_t port = 0, const char *address = "0.0.0.0",
                            int backlog = 16) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            throw TransportError("socket() failed");
        }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr = UdpEndpoint::ipv4(address, port);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(fd, backlog) != 0) {
            detail::close_fd(fd);
            throw TransportError("tcp bind/listen failed");
        }
        TcpListener l;
        l.fd_ = fd;
        return l;
    }

    // Accept a connection within timeout_ms, or std::nullopt on timeout.
    std::optional<TcpConnection> accept(int timeout_ms) {
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) != 1) {
            return std::nullopt;
        }
        sockaddr_in src;
        socklen_t len = sizeof(src);
        int cfd = ::accept(fd_, reinterpret_cast<sockaddr *>(&src), &len);
        if (cfd < 0) {
            return std::nullopt;
        }
        return TcpConnection::from_fd(cfd);
    }

    uint16_t local_port() const {
        sockaddr_in a;
        socklen_t len = sizeof(a);
        if (::getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len) == 0) {
            return ntohs(a.sin_port);
        }
        return 0;
    }

    int fileno() const { return fd_; }
    void close() { detail::close_fd(fd_); }

private:
    int fd_;
};

} // namespace someip

#endif // SOMEIP_TRANSPORT_HPP
// C++ v2 transport layer tests (loopback; mirrors tests/test_py_transport.py).
#include "someip/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

#define CHECK_THROWS(expr)                                                \
    do {                                                                  \
        bool threw = false;                                               \
        try { (void)(expr); }                                             \
        catch (const someip::TransportError &) { threw = true; }          \
        CHECK(threw);                                                     \
    } while (0)

static someip::Message make_msg(uint16_t svc, uint16_t method, uint8_t type,
                                const std::vector<uint8_t> &payload) {
    someip::Message m;
    m.header.service_id = svc;
    m.header.method_id = method;
    m.header.client_id = 0x0001;
    m.header.session_id = 0x0001;
    m.header.message_type = type;
    m.payload = payload;
    return m;
}

static void test_udp_loopback() {
    someip::UdpEndpoint client = someip::UdpEndpoint::unicast(0, "127.0.0.1");
    someip::UdpEndpoint server = someip::UdpEndpoint::unicast(0, "127.0.0.1");
    sockaddr_in dst = someip::UdpEndpoint::ipv4("127.0.0.1", server.local_port());
    const uint8_t ping[] = {'p', 'i', 'n', 'g'};
    ssize_t n = client.send(ping, 4, dst);
    CHECK(n == 4);
    std::vector<uint8_t> got;
    sockaddr_in src;
    bool ok = server.recv(got, src, 1000);
    CHECK(ok);
    CHECK(got.size() == 4 && std::memcmp(got.data(), ping, 4) == 0);
    client.close();
    server.close();
}

static void test_udp_timeout() {
    someip::UdpEndpoint e = someip::UdpEndpoint::unicast(0, "127.0.0.1");
    std::vector<uint8_t> got;
    sockaddr_in src;
    bool ok = e.recv(got, src, 50);
    CHECK(!ok);
    e.close();
}

static void test_udp_send_after_close() {
    someip::UdpEndpoint e = someip::UdpEndpoint::unicast(0, "127.0.0.1");
    e.close();
    sockaddr_in dst = someip::UdpEndpoint::ipv4("127.0.0.1", 1);
    const uint8_t x[] = {'x'};
    CHECK_THROWS(e.send(x, 1, dst));
}

static void test_tcp_request_response() {
    someip::TcpListener listener = someip::TcpListener::bind(0x8000, "127.0.0.1");
    const uint16_t port = listener.local_port();
    std::vector<uint8_t> server_payload = {0, 0, 0, 7};

    std::thread server([&] {
        auto opt = listener.accept(2000);
        CHECK(opt.has_value());
        if (!opt) {
            return;
        }
        someip::TcpConnection conn = std::move(*opt);
        try {
            while (true) {
                auto frame = conn.recv_frame(2000);
                if (!frame) {
                    break;
                }
                someip::Message resp = make_msg(0x1234, frame->header.method_id, 0x80,
                                                server_payload);
                resp.header.client_id = frame->header.client_id;
                resp.header.session_id = frame->header.session_id;
                conn.send_frame(resp);
            }
        } catch (const someip::ReceiveTimeout &) {
        }
        conn.close();
    });

    try {
        someip::TcpConnection cli = someip::TcpConnection::connect("127.0.0.1", port, 2000);
        someip::Message req = make_msg(0x1234, 0x0002, 0x00,
                                       {0, 0, 0, 3, 0, 0, 0, 4});
        cli.send_frame(req);
        auto resp = cli.recv_frame(2000);
        CHECK(resp.has_value());
        CHECK(resp->header.return_code == 0);
        CHECK(resp->payload.size() == 4 && resp->payload[3] == 7);
        cli.close();
    } catch (const std::exception &ex) {
        std::printf("FAIL client side: %s\n", ex.what());
        ++failures;
    }
    server.join();
    listener.close();
}

static void test_tcp_split_frame_reassembly() {
    someip::TcpListener listener = someip::TcpListener::bind(0x8001, "127.0.0.1");
    const uint16_t port = listener.local_port();

    std::thread server([&] {
        auto opt = listener.accept(2000);
        if (!opt) {
            return;
        }
        someip::TcpConnection conn = std::move(*opt);
        std::vector<uint8_t> payload(300, 0xAA);
        someip::Message m = make_msg(0x1234, 0x0001, 0x00, payload);
        std::vector<uint8_t> raw;
        m.to_bytes(raw);
        for (size_t i = 0; i < raw.size(); i += 7) {  // deliberately split
            conn.send_raw(raw.data() + i,
                          std::min<size_t>(7, raw.size() - i));
        }
        conn.close();
    });

    try {
        someip::TcpConnection cli = someip::TcpConnection::connect("127.0.0.1", port, 2000);
        auto frame = cli.recv_frame(2000);
        CHECK(frame.has_value());
        CHECK(frame->payload.size() == 300);
        bool all_aa = true;
        for (uint8_t b : frame->payload) {
            all_aa = all_aa && b == 0xAA;
        }
        CHECK(all_aa);
        cli.close();
    } catch (const std::exception &ex) {
        std::printf("FAIL client side: %s\n", ex.what());
        ++failures;
    }
    server.join();
    listener.close();
}

int main() {
    test_udp_loopback();
    test_udp_timeout();
    test_udp_send_after_close();
    test_tcp_request_response();
    test_tcp_split_frame_reassembly();
    if (failures == 0) {
        std::printf("C++ v2 transport: ALL PASSED\n");
        return 0;
    }
    std::printf("C++ v2 transport: %d failure(s)\n", failures);
    return 1;
}
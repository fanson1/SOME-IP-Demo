// C++ v2 SOME/IP-TP tests (mirrors tests/test_py_tpc.py).
#include "someip/tpc.hpp"

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
        catch (const someip::TpError &) { threw = true; }                 \
        CHECK(threw);                                                     \
    } while (0)

static someip::Message make_msg(const std::vector<uint8_t> &payload,
                                uint8_t type = 0x00,
                                uint16_t session = 0x2222) {
    someip::Message m;
    m.header.service_id = 0x1234;
    m.header.method_id = 0x0002;
    m.header.client_id = 0x1111;
    m.header.session_id = session;
    m.header.message_type = type;
    m.payload = payload;
    return m;
}

static void test_tp_header_golden() {
    someip::TpHeader h0;
    uint8_t b0[4];
    h0.to_bytes(b0);
    CHECK(std::memcmp(b0, "\x00\x00\x00\x00", 4) == 0);

    someip::TpHeader h1;
    h1.more = true;
    h1.to_bytes(b0);
    CHECK(std::memcmp(b0, "\x00\x00\x80\x00", 4) == 0);

    someip::TpHeader h2;
    h2.more = true;
    h2.offset = 1392;  // 0x570
    h2.to_bytes(b0);
    CHECK(std::memcmp(b0, "\x00\x00\x85\x70", 4) == 0);

    someip::TpHeader r = someip::TpHeader::from_bytes(b0, 4);
    CHECK(r.more == true && r.offset == 1392);
}

static void test_tp_header_reserved_bits() {
    uint8_t bad[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_THROWS(someip::TpHeader::from_bytes(bad, 4));
}

static void test_small_untouched() {
    someip::Message small = make_msg(std::vector<uint8_t>(100, 0));
    auto frames = someip::segment(small);
    CHECK(frames.size() == 1);
    CHECK(!frames[0].header.is_tp());
}

static void test_two_segments() {
    std::vector<uint8_t> payload;
    for (int i = 0; i < 2000; ++i) {
        payload.push_back(uint8_t(i));
    }
    someip::Message orig = make_msg(payload, 0x80);
    auto frames = someip::segment(orig);
    CHECK(frames.size() == 2);

    CHECK(frames[0].header.message_type == 0x21);  // TP_RESPONSE
    someip::TpHeader tp0 = someip::TpHeader::from_bytes(frames[0].payload.data(),
                                                        frames[0].payload.size());
    CHECK(tp0.more);
    CHECK(tp0.offset == 0);
    CHECK(frames[0].payload.size() == 4 + 1392);
    CHECK(std::memcmp(frames[0].payload.data() + 4, payload.data(), 1392) == 0);

    someip::TpHeader tp1 = someip::TpHeader::from_bytes(frames[1].payload.data(),
                                                        frames[1].payload.size());
    CHECK(!tp1.more);
    CHECK(tp1.offset == 1392);
    CHECK(frames[1].payload.size() == 4 + (2000 - 1392));
    CHECK(std::memcmp(frames[1].payload.data() + 4, payload.data() + 1392, 608) == 0);
}

static void test_roundtrip_sizes() {
    for (size_t size : {size_t(1), size_t(1392), size_t(1393), size_t(2000),
                        size_t(2784), size_t(3000), size_t(4096)}) {
        std::vector<uint8_t> payload;
        for (size_t i = 0; i < size; ++i) {
            payload.push_back(uint8_t((i * 7) & 0xFF));
        }
        someip::Message orig = make_msg(payload);
        someip::Reassembler reass;
        std::optional<someip::Message> got;
        for (auto &f : someip::segment(orig)) {
            got = reass.add(f);
        }
        CHECK(got.has_value());
        CHECK(got->payload.size() == size);
        CHECK(std::memcmp(got->payload.data(), payload.data(), size) == 0);
        CHECK(got->header.message_type == 0x00);
        CHECK(got->header.request_id() == orig.header.request_id());
    }
}

static void test_passthrough() {
    someip::Reassembler reass;
    someip::Message small = make_msg({1, 2, 3});
    auto got = reass.add(small);
    CHECK(got.has_value());
    CHECK(got->payload.size() == 3);
}

static void test_interleaved_sessions() {
    std::vector<uint8_t> a(3000, 0xAA);
    std::vector<uint8_t> b(3000, 0xBB);
    auto sa = someip::segment(make_msg(a, 0x00, 0x0001));
    auto sb = someip::segment(make_msg(b, 0x80, 0x0002));
    someip::Reassembler reass;
    bool got_a = false;
    std::optional<someip::Message> got_b;
    for (size_t i = 0; i < sa.size(); ++i) {
        if (reass.add(sa[i])) {
            got_a = true;
        }
        if (auto r = reass.add(sb[i])) {
            got_b = r;
        }
    }
    CHECK(got_a);
    CHECK(got_b.has_value());
    CHECK(got_b->payload.size() == 3000 && got_b->payload[0] == 0xBB);
    CHECK(got_b->header.message_type == 0x80);
}

static void test_out_of_order_raises() {
    auto frames = someip::segment(make_msg(std::vector<uint8_t>(3000, 0x11)));
    someip::Reassembler reass;
    CHECK(!reass.add(frames[0]));
    CHECK_THROWS(reass.add(frames.back()));  // skips middle segment
}

static void test_timeout_eviction() {
    auto frames = someip::segment(make_msg(std::vector<uint8_t>(3000, 0x22)));
    someip::Reassembler reass(0.1);
    CHECK(!reass.add(frames[0]));
    CHECK(reass.size() == 1);
    reass.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    reass.tick();
    CHECK(reass.size() == 0);
}

int main() {
    test_tp_header_golden();
    test_tp_header_reserved_bits();
    test_small_untouched();
    test_two_segments();
    test_roundtrip_sizes();
    test_passthrough();
    test_interleaved_sessions();
    test_out_of_order_raises();
    test_timeout_eviction();
    if (failures == 0) {
        std::printf("C++ v2 tpc: ALL PASSED\n");
        return 0;
    }
    std::printf("C++ v2 tpc: %d failure(s)\n", failures);
    return 1;
}
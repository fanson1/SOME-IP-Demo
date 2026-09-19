// C++ v2 app-layer tests (mirrors tests/test_py_app.py): threaded service +
// client over real UDP/SD on loopback LAN, covering RPC, unknown methods,
// fields (get/set/notify), eventgroup subscriptions, and SOME/IP-TP.
#include "someip/app.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using someip::app::ClientV2;
using someip::app::SomeipServiceV2;
using someip::Message;
using someip::MessageType;
using someip::ReturnCode;

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static bool wait_until(std::function<bool()> pred, double seconds) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(seconds));
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

static std::vector<uint8_t> concat(std::vector<uint8_t> a,
                                   const std::vector<uint8_t> &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

static void test_rpc_and_unknown_method() {
    const uint16_t svc = 0x1100, inst = 0x0001, sd_port = 31001;
    SomeipServiceV2 s(svc, inst, 0x01, 1, 0, 0, sd_port);
    s.add_method(0x0001, [](const std::vector<uint8_t> &, const sockaddr_in &) {
        return std::make_pair(uint8_t(ReturnCode::E_OK),
                              std::vector<uint8_t>{0x00, 0x00, 0x12, 0x34,
                                                   0x00, 0x00, 0x56, 0x78,
                                                   0x01, 0x00});
    });
    s.add_method(0x0002, [](const std::vector<uint8_t> &p, const sockaddr_in &) {
        if (p.size() != 8) {
            return std::make_pair(uint8_t(ReturnCode::E_MALFORMED_MESSAGE),
                                  std::vector<uint8_t>{});
        }
        const uint32_t a = someip::app::unpack_u32(p, 0);
        const uint32_t b = someip::app::unpack_u32(p, 4);
        return std::make_pair(uint8_t(ReturnCode::E_OK),
                              someip::app::be_u32(a + b));
    });
    s.start();

    ClientV2 c(0x0701, sd_port);
    c.start();
    CHECK(c.wait_for_service(svc, inst, {}, 8.0));
    auto ep = c.discovered_endpoint(svc, inst);
    CHECK(ep.has_value());
    if (ep) {
        CHECK(!ep->first.empty() && ep->second > 0);
    }

    auto r1 = c.request(svc, inst, 0x0001, {}, 3.0);
    CHECK(r1.first == ReturnCode::E_OK);
    CHECK(r1.second.size() == 10);
    if (r1.second.size() == 10) {
        CHECK(r1.second[2] == 0x12 && r1.second[3] == 0x34);
        CHECK(r1.second[6] == 0x56 && r1.second[7] == 0x78);
        CHECK(r1.second[8] == 0x01 && r1.second[9] == 0x00);
    }

    auto r2 = c.request(svc, inst, 0x0002,
                        concat(someip::app::be_u32(3), someip::app::be_u32(4)),
                        3.0);
    CHECK(r2.first == ReturnCode::E_OK);
    if (r2.second.size() == 4) {
        CHECK(someip::app::unpack_u32(r2.second, 0) == 7);
    }

    auto r3 = c.request(svc, inst, 0x0F00, {}, 3.0);
    CHECK(r3.first == ReturnCode::E_UNKNOWN_METHOD);

    c.stop();
    s.stop();
    std::printf("  app rpc/unknown: done\n");
}

static void test_field_get_set_notify() {
    const uint16_t svc = 0x1101, inst = 0x0001, sd_port = 31002;
    const uint16_t FIELD = 0x1000, EG = 0x0001;
    SomeipServiceV2 s(svc, inst, 0x01, 1, 0, 0, sd_port);
    s.add_uint32_field(FIELD, EG, 0);
    s.start();

    ClientV2 c(0x0702, sd_port);
    bool got_notify = false;
    std::vector<uint8_t> notify_payload;
    c.on_event(FIELD + 2,
               [&](uint16_t, const std::vector<uint8_t> &p) {
                   got_notify = true;
                   notify_payload = p;
               });
    c.start();

    CHECK(c.wait_for_service(svc, inst, {EG}, 8.0));
    CHECK(c.subscribe(svc, inst, {EG}, 5.0));

    auto r_get = c.request(svc, inst, FIELD, {}, 3.0);
    CHECK(r_get.first == ReturnCode::E_OK);
    if (r_get.second.size() == 4) {
        CHECK(someip::app::unpack_u32(r_get.second, 0) == 0);
    }

    auto r_set = c.request(svc, inst, FIELD + 1, someip::app::be_u32(88), 3.0);
    CHECK(r_set.first == ReturnCode::E_OK);
    CHECK(wait_until([&] { return got_notify; }, 3.0));
    if (got_notify && notify_payload.size() == 4) {
        CHECK(someip::app::unpack_u32(notify_payload, 0) == 88);
    }

    auto r_get2 = c.request(svc, inst, FIELD, {}, 3.0);
    CHECK(r_get2.first == ReturnCode::E_OK);
    if (r_get2.second.size() == 4) {
        CHECK(someip::app::unpack_u32(r_get2.second, 0) == 88);
    }

    c.stop();
    s.stop();
    std::printf("  app field get/set/notify: done\n");
}

static void test_event_notification() {
    const uint16_t svc = 0x1102, inst = 0x0001, sd_port = 31003;
    const uint16_t EV = 0x8001, EG = 0x0001;
    SomeipServiceV2 s(svc, inst, 0x01, 1, 0, 0, sd_port);
    s.add_event(EV, EG);
    s.start();

    ClientV2 c(0x0703, sd_port);
    bool got = false;
    std::vector<uint8_t> payload;
    c.on_event(EV, [&](uint16_t, const std::vector<uint8_t> &p) {
        got = true;
        payload = p;
    });
    c.start();

    CHECK(c.wait_for_service(svc, inst, {EG}, 8.0));
    CHECK(c.subscribe(svc, inst, {EG}, 5.0));

    s.publish_event(EV, someip::app::be_u32(0xCAFEBABE));
    CHECK(wait_until([&] { return got; }, 3.0));
    if (got && payload.size() == 4) {
        CHECK(someip::app::unpack_u32(payload, 0) == 0xCAFEBABE);
    }

    c.stop();
    s.stop();
    std::printf("  app event notification: done\n");
}

static void test_tp_roundtrip() {
    const uint16_t svc = 0x1103, inst = 0x0001, sd_port = 31004;
    constexpr size_t N = 4000;  // > 1392 no-TP ceiling
    SomeipServiceV2 s(svc, inst, 0x01, 1, 0, 0, sd_port);
    s.add_method(0x0020, [](const std::vector<uint8_t> &p, const sockaddr_in &) {
        return std::make_pair(uint8_t(ReturnCode::E_OK), p);  // echo
    });
    s.start();

    std::vector<uint8_t> send(N);
    for (size_t i = 0; i < N; ++i) {
        send[i] = uint8_t(i * 7 + 1);
    }

    ClientV2 c(0x0704, sd_port);
    c.start();
    CHECK(c.wait_for_service(svc, inst, {}, 8.0));

    auto r = c.request(svc, inst, 0x0020, send, 6.0);
    CHECK(r.first == ReturnCode::E_OK);
    CHECK(r.second.size() == N);
    if (r.second.size() == N) {
        bool all_match = true;
        for (size_t i = 0; i < N; ++i) {
            if (r.second[i] != send[i]) {
                all_match = false;
                break;
            }
        }
        CHECK(all_match);
    }

    c.stop();
    s.stop();
    std::printf("  app TP roundtrip: done\n");
}

int main() {
    test_rpc_and_unknown_method();
    test_field_get_set_notify();
    test_event_notification();
    test_tp_roundtrip();
    if (failures == 0) {
        std::printf("C++ v2 app: ALL PASSED\n");
        std::printf("  (a few seconds of real SD discovery per test)\n");
        return 0;
    }
    std::printf("C++ v2 app: %d failure(s)\n", failures);
    return 1;
}
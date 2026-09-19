// SOME/IP demo service (v2 app layer, mirrors platform/python/service_demo.py).
#include "someip/app.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <thread>
#include <vector>

using someip::MessageType;
using someip::ReturnCode;

static constexpr uint16_t SERVICE_ID = 0x1234;
static constexpr uint16_t INSTANCE_ID = 0x5678;
static constexpr uint16_t METHOD_GET_VERSION = 0x0001;
static constexpr uint16_t METHOD_ADD = 0x0002;
static constexpr uint16_t FIELD_SPEED = 0x1000;
static constexpr uint16_t EVENT_STATUS = 0x8001;
static constexpr uint16_t EVENTGROUP_MAIN = 0x0001;

int main() {
    someip::app::SomeipServiceV2 service(
        SERVICE_ID, INSTANCE_ID, 0x01, 0x00000001, 30500, 30501);

    service.add_method(
        METHOD_GET_VERSION, [](const std::vector<uint8_t> &, const sockaddr_in &) {
            std::vector<uint8_t> p(10);
            p[0] = uint8_t(SERVICE_ID >> 24);
            p[1] = uint8_t(SERVICE_ID >> 16);
            p[2] = uint8_t(SERVICE_ID >> 8);
            p[3] = uint8_t(SERVICE_ID);
            p[4] = uint8_t(INSTANCE_ID >> 24);
            p[5] = uint8_t(INSTANCE_ID >> 16);
            p[6] = uint8_t(INSTANCE_ID >> 8);
            p[7] = uint8_t(INSTANCE_ID);
            p[8] = 1;  // major
            p[9] = 0;  // minor
            return std::make_pair(uint8_t(ReturnCode::E_OK), std::move(p));
        });

    service.add_method(
        METHOD_ADD, [](const std::vector<uint8_t> &payload, const sockaddr_in &) {
            if (payload.size() != 8) {
                return std::make_pair(uint8_t(ReturnCode::E_MALFORMED_MESSAGE),
                                      std::vector<uint8_t>{});
            }
            const uint32_t a = someip::app::unpack_u32(payload, 0);
            const uint32_t b = someip::app::unpack_u32(payload, 4);
            return std::make_pair(uint8_t(ReturnCode::E_OK),
                                  someip::app::be_u32(a + b));
        });

    service.add_uint32_field(FIELD_SPEED, EVENTGROUP_MAIN, 0);
    service.add_event(EVENT_STATUS, EVENTGROUP_MAIN);
    service.start();

    std::printf("SOME/IP Service (v2) started:\n");
    std::printf("  service_id    = 0x%04X\n", SERVICE_ID);
    std::printf("  instance_id   = 0x%04X\n", INSTANCE_ID);
    std::printf("  method        = udp %s:%u\n", service.interface_ip().c_str(),
                service.method_port());
    std::printf("  event         = udp %s:%u\n", service.interface_ip().c_str(),
                service.event_port());
    std::printf("  sd            = %s:%u\n", "224.244.224.245",
                service.sd_port());
    std::printf("  methods       = GetVersion(0x0001), Add(0x0002)\n");
    std::printf("  field         = Speed(0x1000, getter/setter/notifier)\n");
    std::printf("  event         = Status(0x8001)\n");
    std::printf("  eventgroup    = 0x0001\n");
    std::printf("Waiting for clients... (Ctrl+C to quit)\n");
    std::fflush(stdout);

    uint32_t speed = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        speed = (speed + 10) % 220;
        service.set_field(FIELD_SPEED, speed);
        const auto ts = uint32_t(std::time(nullptr));
        std::vector<uint8_t> ev = someip::app::be_u32(ts);
        const auto speed_bytes = someip::app::be_u32(speed);
        ev.insert(ev.end(), speed_bytes.begin(), speed_bytes.end());
        service.publish_event(EVENT_STATUS, ev);
    }
    service.stop();
    return 0;
}
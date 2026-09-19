// SOME/IP demo service (v2 app layer, mirrors platform/python/service_demo.py).
#include "someip/app.hpp"
#include "someip/config.hpp"
#include "someip/log.hpp"
#include "someip/watchdog.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

using someip::MessageType;
using someip::ReturnCode;

static constexpr uint16_t DEFAULT_SERVICE_ID = 0x1234;
static constexpr uint16_t DEFAULT_INSTANCE_ID = 0x5678;
static constexpr uint16_t METHOD_GET_VERSION = 0x0001;
static constexpr uint16_t METHOD_ADD = 0x0002;
static constexpr uint16_t FIELD_SPEED = 0x1000;
static constexpr uint16_t EVENT_STATUS = 0x8001;
static constexpr uint16_t EVENTGROUP_MAIN = 0x0001;

int main(int argc, char **argv) {
    someip::AppConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "-c" || a == "--config") && i + 1 < argc) {
            cfg.load(argv[++i]);
        }
    }

    const uint16_t service_id = cfg.service.present
                                    ? uint16_t(cfg.service.service_id)
                                    : DEFAULT_SERVICE_ID;
    const uint16_t instance_id = cfg.service.present
                                     ? uint16_t(cfg.service.instance_id)
                                     : DEFAULT_INSTANCE_ID;
    const uint8_t major = cfg.service.present ? cfg.service.major : 0x01;
    const uint32_t minor = cfg.service.present ? cfg.service.minor : 1;
    someip::log::Logger logger("service_demo",
                              cfg.log_level.empty()
                                  ? someip::log::default_level()
                                  : someip::log::level_from_name(cfg.log_level));

    someip::app::SomeipServiceV2 service(
        service_id, instance_id, major, minor,
        cfg.service.present ? cfg.service.method_port : 30500,
        cfg.service.present ? cfg.service.event_port : 30501,
        cfg.sd.port, cfg.service.interface);

    service.add_method(
        METHOD_GET_VERSION, [=](const std::vector<uint8_t> &, const sockaddr_in &) {
            std::vector<uint8_t> p(10);
            const uint32_t sid = service_id;
            const uint32_t iid = instance_id;
            p[0] = uint8_t(sid >> 24);
            p[1] = uint8_t(sid >> 16);
            p[2] = uint8_t(sid >> 8);
            p[3] = uint8_t(sid);
            p[4] = uint8_t(iid >> 24);
            p[5] = uint8_t(iid >> 16);
            p[6] = uint8_t(iid >> 8);
            p[7] = uint8_t(iid);
            p[8] = major;
            p[9] = uint8_t(minor);
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
    std::printf("  service_id    = 0x%04X\n", service_id);
    std::printf("  instance_id   = 0x%04X\n", instance_id);
    std::printf("  method        = udp %s:%u\n", service.interface_ip().c_str(),
                service.method_port());
    std::printf("  event         = udp %s:%u\n", service.interface_ip().c_str(),
                service.event_port());
    std::printf("  sd            = %s:%u\n", cfg.sd.multicast.c_str(),
                service.sd_port());
    std::printf("  methods       = GetVersion(0x0001), Add(0x0002)\n");
    std::printf("  field         = Speed(0x1000, getter/setter/notifier)\n");
    std::printf("  event         = Status(0x8001)\n");
    std::printf("  eventgroup    = 0x0001\n");
    std::printf("Waiting for clients... (Ctrl+C to quit)\n");
    std::fflush(stdout);
    logger.info("listening method=%u event=%u sd=%u", service.method_port(),
                service.event_port(), service.sd_port());

    uint32_t speed = 0;
    someip::watchdog::Watchdog wd(5.0, [&] { logger.error("publish loop stalled"); }, 0.2);
    wd.start();
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        wd.pet();
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
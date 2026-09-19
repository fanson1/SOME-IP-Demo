// SOME/IP demo client (v2 app layer, mirrors platform/python/client_demo.py).
#include "someip/app.hpp"
#include "someip/config.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static constexpr uint16_t SERVICE_ID = 0x1234;
static constexpr uint16_t INSTANCE_ID = 0x5678;
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
    someip::app::ClientV2 client(
        cfg.client.present ? cfg.client.client_id : 0x0001,
        cfg.client.present ? cfg.client.sd_port : cfg.sd.port,
        cfg.client.interface);
    client.on_event(EVENT_STATUS,
                    [](uint16_t event_id, const std::vector<uint8_t> &p) {
                        if (p.size() != 8) {
                            return;
                        }
                        const uint32_t ts = someip::app::unpack_u32(p, 0);
                        const uint32_t speed = someip::app::unpack_u32(p, 4);
                        std::printf("  [event 0x%04X] status: ts=%u speed=%u km/h\n",
                                    event_id, ts, speed);
                        std::fflush(stdout);
                    });
    client.on_event(FIELD_SPEED + 2,
                    [](uint16_t event_id, const std::vector<uint8_t> &p) {
                        if (p.size() != 4) {
                            return;
                        }
                        std::printf("  [notify 0x%04X] speed field = %u km/h\n",
                                    event_id, someip::app::unpack_u32(p, 0));
                        std::fflush(stdout);
                    });
    client.start();

    std::printf("Searching service 0x%04X/0x%04X ...\n", SERVICE_ID,
                INSTANCE_ID);
    if (!client.wait_for_service(SERVICE_ID, INSTANCE_ID,
                                 {EVENTGROUP_MAIN}, 10.0)) {
        std::printf("Service not found, is service_demo running?\n");
        client.stop();
        return 1;
    }
    auto ep = client.discovered_endpoint(SERVICE_ID, INSTANCE_ID);
    std::printf("Discovered service at %s:%u\n", ep->first.c_str(),
                ep->second);

    auto r_ver = client.request(SERVICE_ID, INSTANCE_ID, METHOD_GET_VERSION);
    if (r_ver.first != 0) {
        std::printf("GetVersion failed, return code = 0x%02X\n", r_ver.first);
        client.stop();
        return 1;
    }
    if (r_ver.second.size() == 10) {
        std::printf("GetVersion -> service=0x%04X instance=0x%04X v%d.%d\n",
                    (r_ver.second[2] << 8) | r_ver.second[3],
                    (r_ver.second[6] << 8) | r_ver.second[7],
                    r_ver.second[8], r_ver.second[9]);
    }

    std::vector<uint8_t> add_args = someip::app::be_u32(3);
    const auto add_b = someip::app::be_u32(4);
    add_args.insert(add_args.end(), add_b.begin(), add_b.end());
    auto r_add = client.request(SERVICE_ID, INSTANCE_ID, METHOD_ADD, add_args);
    uint32_t result = r_add.second.size() == 4
                          ? someip::app::unpack_u32(r_add.second, 0)
                          : 0;
    std::printf("Add(3, 4) -> rc=0x%02X result=%u\n", r_add.first, result);

    auto r_speed = client.request(SERVICE_ID, INSTANCE_ID, FIELD_SPEED);
    uint32_t speed_value = r_speed.second.size() == 4
                               ? someip::app::unpack_u32(r_speed.second, 0)
                               : 0;
    std::printf("Read Speed  -> rc=0x%02X value=%u km/h\n", r_speed.first,
                speed_value);

    if (client.subscribe(SERVICE_ID, INSTANCE_ID, {EVENTGROUP_MAIN})) {
        std::printf("Subscribed eventgroup 0x%04X, listening...\n",
                    EVENTGROUP_MAIN);
    } else {
        std::printf("Subscribe eventgroup failed\n");
    }

    auto r_write = client.request(SERVICE_ID, INSTANCE_ID, FIELD_SPEED + 1,
                                  someip::app::be_u32(88));
    std::printf("Write Speed(88) -> rc=0x%02X\n", r_write.first);

    auto r_speed2 = client.request(SERVICE_ID, INSTANCE_ID, FIELD_SPEED);
    uint32_t speed2 = r_speed2.second.size() == 4
                          ? someip::app::unpack_u32(r_speed2.second, 0)
                          : 0;
    std::printf("Read Speed  -> rc=0x%02X value=%u km/h\n", r_speed2.first,
                speed2);

    for (int i = 0; i < 12; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    client.stop();
    std::printf("Done.\n");
    return 0;
}
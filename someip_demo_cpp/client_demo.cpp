#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>

#include "someip.hpp"
#include "sd.hpp"
#include "net.hpp"

using someip::Message;
using someip::MessageType;
using someip::ReturnCode;

const uint16_t SERVICE_ID = 0x1234;
const uint16_t INSTANCE_ID = 0x5678;
const uint8_t  MAJOR_VERSION = 0x01;

const uint16_t METHOD_GET_VERSION = 0x0001;
const uint16_t METHOD_ADD = 0x0002;
const uint16_t FIELD_SPEED = 0x1000;
const uint16_t EVENT_STATUS = 0x8001;
const uint16_t EVENTGROUP_MAIN = 0x0001;

static uint16_t g_client_id = 0x0001;
static uint16_t g_session = 0;
static std::mutex g_lock;
static bool g_running = true;

static uint16_t next_session() {
    std::lock_guard<std::mutex> lk(g_lock);
    g_session = (g_session + 1) & 0xFFFF;
    return g_session;
}

static void event_thread(int event_fd) {
    uint8_t buf[65536];
    while (g_running) {
        ssize_t n = recv(event_fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_running) continue;
            break;
        }
        Message msg;
        if (!Message::from_buf(buf, size_t(n), msg)) continue;
        if (msg.message_type != MessageType::NOTIFICATION) continue;
        if (msg.method_id == EVENT_STATUS && msg.payload.size() >= 8) {
            uint32_t ts = someip::load_u32(msg.payload.data());
            uint32_t speed = someip::load_u32(msg.payload.data() + 4);
            printf("  [event 0x%04X] status: ts=%u speed=%u km/h\n",
                   msg.method_id, ts, speed);
        } else if (msg.method_id == FIELD_SPEED + 2 && msg.payload.size() >= 4) {
            uint32_t speed = someip::load_u32(msg.payload.data());
            printf("  [notify 0x%04X] speed field = %u km/h\n",
                   msg.method_id, speed);
        } else {
            printf("  [event 0x%04X] unknown, %zu bytes\n",
                   msg.method_id, msg.payload.size());
        }
    }
}

static std::pair<uint8_t, std::vector<uint8_t>> request(
        int method_fd, const sockaddr_in &endpoint,
        uint16_t method_id, const std::vector<uint8_t> &payload,
        int timeout_ms) {
    uint16_t session = next_session();
    Message req;
    req.service_id = SERVICE_ID;
    req.method_id = method_id;
    req.client_id = g_client_id;
    req.session_id = session;
    req.message_type = MessageType::REQUEST;
    req.return_code = ReturnCode::E_OK;
    req.interface_version = MAJOR_VERSION;
    req.payload = payload;
    std::vector<uint8_t> data;
    req.to_buf(data);
    net::send_to(method_fd, endpoint, data.data(), data.size());

    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(method_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[65536];
    uint32_t want_reqid = (uint32_t(g_client_id) << 16) | session;
    while (g_running) {
        ssize_t n = recvfrom(method_fd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        Message resp;
        if (!Message::from_buf(buf, size_t(n), resp)) continue;
        if (resp.service_id != SERVICE_ID) continue;
        if (resp.request_id() != want_reqid) continue;
        return std::make_pair(resp.return_code, resp.payload);
    }
    return std::make_pair(0xFF, std::vector<uint8_t>());
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string interface_ip;
    if (net::local_ipv4(interface_ip) != 0) interface_ip = "127.0.0.1";

    int method_fd = net::make_udp_socket(0, false);
    int event_fd = net::make_udp_socket(0, true);
    int sd_fd = net::make_sd_socket(interface_ip);
    if (method_fd < 0 || event_fd < 0 || sd_fd < 0) {
        fprintf(stderr, "socket setup failed\n");
        return 1;
    }

    sockaddr_in event_addr;
    socklen_t event_len = sizeof(event_addr);
    getsockname(event_fd, (sockaddr *)&event_addr, &event_len);
    uint16_t event_port = ntohs(event_addr.sin_port);

    std::thread t_events(event_thread, event_fd);

    // ---- SubscribeEventgroup over SD multicast ----
    {
        std::vector<std::vector<uint8_t>> entries;
        std::vector<std::vector<uint8_t>> options;
        std::vector<uint8_t> entry;
        sd::build_entry(sd::EntryType::SUBSCRIBE_EVENTGROUP, SERVICE_ID,
                        INSTANCE_ID, MAJOR_VERSION, sd::SD_TTL, 0,
                        EVENTGROUP_MAIN, 0, 1, entry);
        entries.push_back(entry);
        std::vector<uint8_t> option;
        sd::build_ipv4_endpoint_option(interface_ip, event_port, option);
        options.push_back(option);
        std::vector<uint8_t> data;
        uint16_t session = next_session();
        sd::build_sd_message(entries, options, session, data);
        sockaddr_in group = net::make_addr(sd::SD_MULTICAST_ADDRESS, sd::SD_PORT);
        net::send_to(sd_fd, group, data.data(), data.size());
        printf("Sent SubscribeEventgroup 0x%04X, waiting for ack...\n",
               EVENTGROUP_MAIN);
        // wait for ack (may also see offers), deadline = 3s
        uint8_t buf[65536];
        bool acked = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!acked && std::chrono::steady_clock::now() < deadline) {
            timeval tv;
            int64_t rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
            tv.tv_sec = rem_ms / 1000;
            tv.tv_usec = (rem_ms % 1000) * 1000;
            setsockopt(sd_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t n = recvfrom(sd_fd, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n < 0) continue;
            Message sdmsg;
            if (!Message::from_buf(buf, size_t(n), sdmsg)) continue;
            if (sdmsg.service_id != sd::SD_SERVICE_ID) continue;
            const uint8_t *p = sdmsg.payload.data();
            size_t plen = sdmsg.payload.size();
            if (plen < 4) continue;
            uint32_t entries_len = someip::load_u32(p);
            const uint8_t *entries_ptr = p + 4;
            for (uint32_t i = 0; i + 16 <= entries_len; i += 16) {
                const uint8_t *e = entries_ptr + i;
                if (e[0] != sd::EntryType::SUBSCRIBE_EVENTGROUP_ACK) continue;
                if (someip::load_u16(e + 4) != SERVICE_ID) continue;
                uint16_t eg = someip::load_u16(e + 12);
                if (eg == EVENTGROUP_MAIN) {
                    acked = true;
                    printf("SubscribeEventgroupAck received\n");
                    break;
                }
            }
        }
        if (!acked) printf("SubscribeEventgroup ack not received (ttl=?)\n");
    }

    printf("Searching service 0x%04X/0x%04X ...\n", SERVICE_ID, INSTANCE_ID);
    std::vector<std::pair<std::string, uint16_t>> offers;
    {
        std::vector<std::vector<uint8_t>> entries;
        entries.push_back([&]() {
            std::vector<uint8_t> e;
            sd::build_entry(sd::EntryType::FIND_SERVICE, SERVICE_ID, INSTANCE_ID,
                            MAJOR_VERSION, 0, 0, 0, 0, 0, e);
            return e;
        }());
        std::vector<std::vector<uint8_t>> options;
        std::vector<uint8_t> data;
        sd::build_sd_message(entries, options, next_session(), data);
        sockaddr_in group = net::make_addr(sd::SD_MULTICAST_ADDRESS, sd::SD_PORT);
        for (int i = 0; i < 3; ++i)
            net::send_to(sd_fd, group, data.data(), data.size());
    }
    timeval tv;
    tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(sd_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[65536];
    while (offers.empty()) {
        ssize_t n = recvfrom(sd_fd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n < 0) break;
        Message sdmsg;
        if (!Message::from_buf(buf, size_t(n), sdmsg)) continue;
        if (sdmsg.service_id != sd::SD_SERVICE_ID) continue;
        const uint8_t *p = sdmsg.payload.data();
        size_t plen = sdmsg.payload.size();
        if (plen < 4) continue;
        uint32_t entries_len = someip::load_u32(p);
        const uint8_t *entries_ptr = p + 4;
        size_t o = 4 + entries_len;
        uint32_t options_len = someip::load_u32(p + o);
        const uint8_t *opts = p + o + 4;
        size_t oc = 0;
        while (oc + 4 <= options_len) {
            uint16_t olen = someip::load_u16(opts + oc);
            if (opts[oc + 2] == sd::OptionType::IPV4_ENDPOINT && olen >= 9) {
                char ipbuf[INET_ADDRSTRLEN];
                snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
                         opts[oc + 4], opts[oc + 5], opts[oc + 6], opts[oc + 7]);
                uint16_t port = someip::load_u16(opts + oc + 9);
                for (uint32_t i = 0; i + 16 <= entries_len; i += 16) {
                    const uint8_t *e = entries_ptr + i;
                    if (e[0] == sd::EntryType::OFFER_SERVICE &&
                        someip::load_u16(e + 4) == SERVICE_ID &&
                        someip::load_u16(e + 6) == INSTANCE_ID) {
                        offers.push_back(std::make_pair(std::string(ipbuf), port));
                    }
                }
            }
            oc += 2 + olen;
        }
    }
    if (offers.empty()) {
        printf("Service not found, is the C++ or Python service running?\n");
        g_running = false;
        t_events.join();
        return 1;
    }

    sockaddr_in endpoint = net::make_addr(offers[0].first, offers[0].second);
    printf("Discovered service at %s:%u\n",
           offers[0].first.c_str(), offers[0].second);

    auto rc0 = request(method_fd, endpoint, METHOD_GET_VERSION, {}, 3000);
    if (rc0.first != 0xFF && rc0.second.size() >= 10) {
        uint32_t sid = someip::load_u32(rc0.second.data());
        uint32_t iid = someip::load_u32(rc0.second.data() + 4);
        printf("GetVersion -> service=0x%04X instance=0x%04X v%u.%u\n",
               sid, iid, rc0.second[8], rc0.second[9]);
    } else {
        printf("GetVersion failed timer\n");
    }

    std::vector<uint8_t> add_payload;
    someip::push_u32(add_payload, 3);
    someip::push_u32(add_payload, 4);
    auto rc1 = request(method_fd, endpoint, METHOD_ADD, add_payload, 3000);
    printf("Add(3, 4) -> rc=0x%02X", rc1.first);
    if (rc1.first == 0 && rc1.second.size() >= 4)
        printf(" result=%u", someip::load_u32(rc1.second.data()));
    printf("\n");

    auto rc2 = request(method_fd, endpoint, FIELD_SPEED, {}, 3000);
    printf("Read Speed  -> rc=0x%02X", rc2.first);
    if (rc2.first == 0 && rc2.second.size() >= 4)
        printf(" value=%u km/h", someip::load_u32(rc2.second.data()));
    printf("\n");

    std::vector<uint8_t> speed_payload;
    someip::push_u32(speed_payload, 88);
    auto rc3 = request(method_fd, endpoint, FIELD_SPEED + 1, speed_payload, 3000);
    printf("Write Speed(88) -> rc=0x%02X\n", rc3.first);
    auto rc4 = request(method_fd, endpoint, FIELD_SPEED, {}, 3000);
    printf("Read Speed  -> rc=0x%02X", rc4.first);
    if (rc4.first == 0 && rc4.second.size() >= 4)
        printf(" value=%u km/h", someip::load_u32(rc4.second.data()));
    printf("\n");

    printf("Listening events for 6 seconds...\n");
    std::this_thread::sleep_for(std::chrono::seconds(6));

    g_running = false;
    t_events.join();
    ::close(method_fd);
    ::close(event_fd);
    ::close(sd_fd);
    printf("Done.\n");
    return 0;
}
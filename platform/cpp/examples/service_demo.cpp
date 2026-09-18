#include <map>
#include <set>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>

#include "someip/legacy/someip.hpp"
#include "someip/legacy/sd.hpp"
#include "someip/legacy/net.hpp"

using someip::legacy::Message;
using someip::legacy::MessageType;
using someip::legacy::ReturnCode;

typedef std::function<void(const std::vector<uint8_t> &, const sockaddr_in &, uint8_t &,
                           std::vector<uint8_t> &)> MethodHandler;

const uint16_t SERVICE_ID = 0x1234;
const uint16_t INSTANCE_ID = 0x5678;
const uint8_t  MAJOR_VERSION = 0x01;
const uint32_t MINOR_VERSION = 0x00000001;

const uint16_t METHOD_GET_VERSION = 0x0001;
const uint16_t METHOD_ADD = 0x0002;
const uint16_t FIELD_SPEED = 0x1000;
const uint16_t EVENT_STATUS = 0x8001;
const uint16_t EVENTGROUP_MAIN = 0x0001;

const uint16_t METHOD_PORT = 30500;
const uint16_t EVENT_PORT = 30501;

static uint16_t g_session = 0;
static std::mutex g_lock;
static bool g_running = true;

static uint16_t next_session() {
    std::lock_guard<std::mutex> lk(g_lock);
    g_session = (g_session + 1) & 0xFFFF;
    return g_session;
}

static void offer_once(int sd_fd, const std::string &interface_ip) {
    std::vector<std::vector<uint8_t>> entries;
    std::vector<std::vector<uint8_t>> options;
    std::vector<uint8_t> entry;
    someip::legacy::build_entry(someip::legacy::EntryType::OFFER_SERVICE, SERVICE_ID, INSTANCE_ID,
                    MAJOR_VERSION, someip::legacy::SD_TTL, MINOR_VERSION, 0, 0, 1, entry);
    entries.push_back(entry);
    std::vector<uint8_t> option;
    someip::legacy::build_ipv4_endpoint_option(interface_ip, METHOD_PORT, option);
    options.push_back(option);
    std::vector<uint8_t> data;
    someip::legacy::build_sd_message(entries, options, next_session(), data);
    sockaddr_in group = someip::legacy::make_addr(someip::legacy::SD_MULTICAST_ADDRESS, someip::legacy::SD_PORT);
    someip::legacy::send_to(sd_fd, group, data.data(), data.size());
}

static void offer_thread(int sd_fd, const std::string &interface_ip) {
    while (g_running) {
        offer_once(sd_fd, interface_ip);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
}

struct AddrLess {
    bool operator()(const sockaddr_in &a, const sockaddr_in &b) const {
        if (a.sin_addr.s_addr != b.sin_addr.s_addr)
            return a.sin_addr.s_addr < b.sin_addr.s_addr;
        return a.sin_port < b.sin_port;
    }
};

struct Service {
    std::string interface_ip;
    int method_fd = -1;
    int event_fd = -1;
    int sd_fd = -1;
    std::map<uint16_t, MethodHandler> methods;
    std::map<uint16_t, uint16_t> events;       // event_id -> eventgroup
    std::map<uint16_t, uint32_t> fields;       // field_id -> value
    std::map<uint16_t, std::set<sockaddr_in, AddrLess>> subscribers;

    Service(const std::string &ip) : interface_ip(ip) {}
};

static void publish_event(Service &svc, uint16_t event_id,
                          const std::vector<uint8_t> &payload) {
    auto it = svc.events.find(event_id);
    if (it == svc.events.end()) return;
    uint16_t eventgroup = it->second;
    Message msg;
    msg.service_id = SERVICE_ID;
    msg.method_id = event_id;
    msg.client_id = 0x0000;
    msg.session_id = next_session();
    msg.message_type = MessageType::NOTIFICATION;
    msg.return_code = ReturnCode::E_OK;
    msg.interface_version = MAJOR_VERSION;
    msg.payload = payload;
    std::vector<uint8_t> data;
    msg.to_buf(data);
    auto sit = svc.subscribers.find(eventgroup);
    if (sit == svc.subscribers.end()) return;
    for (const auto &target : sit->second) {
        someip::legacy::send_to(svc.event_fd, target, data.data(), data.size());
    }
}

static void set_uint32_field(Service &svc, uint16_t field_id, uint32_t value) {
    svc.fields[field_id] = value;
    std::vector<uint8_t> payload;
    someip::legacy::push_u32(payload, value);
    publish_event(svc, field_id + 2, payload);
}

static void method_thread(Service &svc) {
    uint8_t buf[65536];
    sockaddr_in from;
    while (g_running) {
        socklen_t len = sizeof(from);
        ssize_t n = recvfrom(svc.method_fd, buf, sizeof(buf), 0,
                             (sockaddr *)&from, &len);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_running) continue;
            break;
        }
        Message req;
        if (!Message::from_buf(buf, size_t(n), req)) continue;
        if (req.service_id != SERVICE_ID) continue;
        if (req.message_type != MessageType::REQUEST &&
            req.message_type != MessageType::REQUEST_NO_RETURN) continue;

        uint8_t rc = ReturnCode::E_OK;
        std::vector<uint8_t> response;
        auto hit = svc.methods.find(req.method_id);
        if (hit != svc.methods.end()) {
            hit->second(req.payload, from, rc, response);
        } else {
            rc = ReturnCode::E_UNKNOWN_METHOD;
        }
        if (req.message_type == MessageType::REQUEST) {
            Message ack;
            ack.service_id = SERVICE_ID;
            ack.method_id = req.method_id;
            ack.client_id = req.client_id;
            ack.session_id = req.session_id;
            ack.message_type = MessageType::RESPONSE;
            ack.return_code = rc;
            ack.interface_version = MAJOR_VERSION;
            ack.payload = response;
            std::vector<uint8_t> data;
            ack.to_buf(data);
            someip::legacy::send_to(svc.method_fd, from, data.data(), data.size());
        }
    }
}

static void sd_thread(Service &svc) {
    uint8_t buf[65536];
    while (g_running) {
        sockaddr_in from;
        socklen_t len = sizeof(from);
        ssize_t n = recvfrom(svc.sd_fd, buf, sizeof(buf), 0,
                             (sockaddr *)&from, &len);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_running) continue;
            break;
        }
        if (size_t(n) < 16) continue;
        // skip our own offers
        Message sdmsg;
        if (!Message::from_buf(buf, size_t(n), sdmsg)) continue;
        if (sdmsg.service_id != someip::legacy::SD_SERVICE_ID) continue;

        const uint8_t *p = sdmsg.payload.data();
        size_t plen = sdmsg.payload.size();
        if (plen < 4) continue;
        uint32_t entries_len = someip::legacy::load_u32(p);
        if (4 + entries_len + 4 > plen) continue;
        const uint8_t *entries = p + 4;
        size_t o = 4 + entries_len;
        uint32_t options_len = someip::legacy::load_u32(p + o);
        const uint8_t *opts = p + o + 4;

        std::vector<std::pair<std::string, uint16_t>> endpoints;
        size_t oc = 0;
        while (oc + 4 <= options_len) {
            uint16_t olen = someip::legacy::load_u16(opts + oc);
            uint8_t otype = opts[oc + 2];
            if (otype == someip::legacy::OptionType::IPV4_ENDPOINT && olen >= 9) {
                char ipbuf[INET_ADDRSTRLEN];
                snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
                         opts[oc + 4], opts[oc + 5], opts[oc + 6], opts[oc + 7]);
                uint16_t port = someip::legacy::load_u16(opts + oc + 9);
                endpoints.push_back(std::make_pair(std::string(ipbuf), port));
            }
            oc += 2 + olen;
        }

        for (uint32_t i = 0; i + 16 <= entries_len; i += 16) {
            const uint8_t *e = entries + i;
            uint8_t etype = e[0];
            uint8_t index_first = e[1];
            uint8_t counts = e[3];
            uint16_t sid = someip::legacy::load_u16(e + 4);
            uint16_t iid = someip::legacy::load_u16(e + 6);
            uint8_t major = e[8];
            if (sid != SERVICE_ID || iid != INSTANCE_ID || major != MAJOR_VERSION)
                continue;
            if (etype == someip::legacy::EntryType::FIND_SERVICE) {
                offer_once(svc.sd_fd, svc.interface_ip);
            } else if (etype == someip::legacy::EntryType::SUBSCRIBE_EVENTGROUP) {
                uint16_t eventgroup = someip::legacy::load_u16(e + 12);
                uint8_t no = counts & 0x0F;
                sockaddr_in sub;
                std::memset(&sub, 0, sizeof(sub));
                bool have = false;
                if (no > 0 && size_t(index_first) < endpoints.size()) {
                    const auto &ep = endpoints[index_first];
                    sub = someip::legacy::make_addr(ep.first, ep.second);
                    have = true;
                }
                if (have) {
                    svc.subscribers[eventgroup].insert(sub);
                    std::vector<std::vector<uint8_t>> ack_entries;
                    std::vector<std::vector<uint8_t>> ack_options;
                    std::vector<uint8_t> ack_entry;
                    someip::legacy::build_entry(someip::legacy::EntryType::SUBSCRIBE_EVENTGROUP_ACK,
                                    SERVICE_ID, INSTANCE_ID, MAJOR_VERSION,
                                    someip::legacy::SD_TTL, 0, eventgroup, 0, 0, ack_entry);
                    ack_entries.push_back(ack_entry);
                    std::vector<uint8_t> ack;
                    someip::legacy::build_sd_message(ack_entries, ack_options, next_session(), ack);
                    sockaddr_in group = someip::legacy::make_addr(someip::legacy::SD_MULTICAST_ADDRESS, someip::legacy::SD_PORT);
                    someip::legacy::send_to(svc.sd_fd, group, ack.data(), ack.size());
                }
            }
        }
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string interface_ip;
    if (someip::legacy::local_ipv4(interface_ip) != 0) interface_ip = "127.0.0.1";

    Service svc(interface_ip);
    svc.method_fd = someip::legacy::make_udp_socket(METHOD_PORT, false);
    svc.event_fd = someip::legacy::make_udp_socket(EVENT_PORT, false);
    svc.sd_fd = someip::legacy::make_sd_socket(interface_ip);
    if (svc.method_fd < 0 || svc.event_fd < 0 || svc.sd_fd < 0) {
        fprintf(stderr, "socket setup failed\n");
        return 1;
    }

    svc.methods[METHOD_GET_VERSION] = [](const std::vector<uint8_t> &, const sockaddr_in &,
                                         uint8_t &rc, std::vector<uint8_t> &out) {
        rc = ReturnCode::E_OK;
        someip::legacy::push_u32(out, SERVICE_ID);
        someip::legacy::push_u32(out, INSTANCE_ID);
        out.push_back(MAJOR_VERSION);
        out.push_back(0x00);
    };

    svc.methods[METHOD_ADD] = [](const std::vector<uint8_t> &payload, const sockaddr_in &,
                                 uint8_t &rc, std::vector<uint8_t> &out) {
        if (payload.size() < 8) {
            rc = ReturnCode::E_MALFORMED_MESSAGE;
            return;
        }
        uint32_t a = someip::legacy::load_u32(payload.data());
        uint32_t b = someip::legacy::load_u32(payload.data() + 4);
        rc = ReturnCode::E_OK;
        someip::legacy::push_u32(out, a + b);
    };

    svc.fields[FIELD_SPEED] = 0;
    uint16_t speed_getter = FIELD_SPEED;
    uint16_t speed_setter = FIELD_SPEED + 1;
    svc.methods[speed_getter] = [&svc](const std::vector<uint8_t> &, const sockaddr_in &,
                                       uint8_t &rc, std::vector<uint8_t> &out) {
        rc = ReturnCode::E_OK;
        someip::legacy::push_u32(out, svc.fields[FIELD_SPEED]);
    };
    svc.methods[speed_setter] = [&svc](const std::vector<uint8_t> &payload, const sockaddr_in &,
                                       uint8_t &rc, std::vector<uint8_t> &out) {
        if (payload.size() < 4) {
            rc = ReturnCode::E_MALFORMED_MESSAGE;
            return;
        }
        uint32_t value = someip::legacy::load_u32(payload.data());
        set_uint32_field(svc, FIELD_SPEED, value);
        rc = ReturnCode::E_OK;
        out = payload;
    };
    svc.events[FIELD_SPEED + 2] = EVENTGROUP_MAIN;
    svc.events[EVENT_STATUS] = EVENTGROUP_MAIN;

    std::thread t_offer(offer_thread, svc.sd_fd, interface_ip);
    std::thread t_method(method_thread, std::ref(svc));
    std::thread t_sd(sd_thread, std::ref(svc));

    printf("SOME/IP Service (C++) started:\n");
    printf("  service_id    = 0x%04X\n", SERVICE_ID);
    printf("  instance_id   = 0x%04X\n", INSTANCE_ID);
    printf("  method        = udp %s:%u\n", interface_ip.c_str(), METHOD_PORT);
    printf("  event         = udp %s:%u\n", interface_ip.c_str(), EVENT_PORT);
    printf("  sd            = %s:%u\n", someip::legacy::SD_MULTICAST_ADDRESS, someip::legacy::SD_PORT);
    printf("  methods       = GetVersion(0x0001), Add(0x0002)\n");
    printf("  field         = Speed(0x1000, getter/setter/notifier)\n");
    printf("  event         = Status(0x8001)\n");
    printf("  eventgroup    = 0x0001\n");
    printf("Waiting for clients... (Ctrl+C to quit)\n");

    uint32_t speed = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        speed = (speed + 10) % 220;
        set_uint32_field(svc, FIELD_SPEED, speed);
        std::vector<uint8_t> payload;
        someip::legacy::push_u32(payload, uint32_t(::time(nullptr)));
        someip::legacy::push_u32(payload, speed);
        publish_event(svc, EVENT_STATUS, payload);
    }

    g_running = false;
    t_offer.join();
    t_method.join();
    t_sd.join();
    ::close(svc.method_fd);
    ::close(svc.event_fd);
    ::close(svc.sd_fd);
    return 0;
}
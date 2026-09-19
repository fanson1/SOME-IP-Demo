#ifndef SOMEIP_SDM_HPP
#define SOMEIP_SDM_HPP

// Service Discovery state machine (byte-compatible with legacy/vSomeIP).
// Mirrors platform/python/someip/sdm.py (C++17, no third-party deps).
//
// Wire layout follows AUTOSAR SOME/IP-SD (PRS 696); TTL lives in entry
// bytes 9..11 (24-bit). Machines never touch sockets: the app layer drives
// process()/handle_datagram()/tick() and ships the returned frames.

#include "wire.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifndef SOMEIP_SD_DEFAULT_TTL
#define SOMEIP_SD_DEFAULT_TTL 3
#endif
#ifndef SOMEIP_SD_CYCLE
#define SOMEIP_SD_CYCLE 2.0
#endif
#ifndef SOMEIP_SD_INITIAL_DELAY_MIN
#define SOMEIP_SD_INITIAL_DELAY_MIN 0.0
#endif
#ifndef SOMEIP_SD_INITIAL_DELAY_MAX
#define SOMEIP_SD_INITIAL_DELAY_MAX 0.1
#endif
#ifndef SOMEIP_SD_REPETITIONS_BASE_DELAY
#define SOMEIP_SD_REPETITIONS_BASE_DELAY 0.2
#endif
#ifndef SOMEIP_SD_REPETITIONS_MAX
#define SOMEIP_SD_REPETITIONS_MAX 3
#endif
#ifndef SOMEIP_SD_REQUESTS_BASE_DELAY
#define SOMEIP_SD_REQUESTS_BASE_DELAY 0.2
#endif

namespace someip {
namespace sdm {

inline constexpr uint16_t SD_PORT = 30490;
inline constexpr const char *SD_MULTICAST_ADDRESS = "224.244.224.245";

static constexpr uint16_t SD_SERVICE_ID = 0xFFFF;
static constexpr uint16_t SD_METHOD_ID = 0x8100;
static constexpr uint8_t SD_INTERFACE_VERSION = 0x01;
static constexpr size_t ENTRY_SIZE = 16;

struct SdEntryType {
    static constexpr uint8_t FIND                  = 0x00;
    static constexpr uint8_t OFFER                 = 0x01;
    static constexpr uint8_t SUBSCRIBE             = 0x06;
    static constexpr uint8_t SUBSCRIBE_ACK         = 0x07;
    static constexpr uint8_t SUBSCRIBE_NACK        = 0x08;
};

struct SdOptionType {
    static constexpr uint16_t IPV4_ENDPOINT        = 0x04;
    static constexpr uint16_t IPV4_MULTICAST       = 0x14;
};

struct SdError : std::runtime_error {
    explicit SdError(const std::string &what) : std::runtime_error(what) {}
};

// ------------------------------------------------------------- codec -----

// Entry: type(1) idx1(1) idx2(1) counts(1) svc(2) inst(2) major(1) ttl(3)
//        tail(4: minor | {eventgroup,reserved}).
inline std::vector<uint8_t> encode_entry(
    uint8_t type, uint16_t service_id, uint16_t instance_id, uint8_t major,
    uint32_t ttl, uint32_t minor = 0, uint16_t eventgroup_id = 0xFFFF,
    uint8_t idx1 = 0, uint8_t idx2 = 0, uint8_t opts1 = 0, uint8_t opts2 = 0) {
    std::vector<uint8_t> e(ENTRY_SIZE);
    e[0] = type;
    e[1] = idx1;
    e[2] = idx2;
    e[3] = uint8_t(((opts2 & 0x0F) << 4) | (opts1 & 0x0F));
    e[4] = uint8_t(service_id >> 8);
    e[5] = uint8_t(service_id);
    e[6] = uint8_t(instance_id >> 8);
    e[7] = uint8_t(instance_id);
    e[8] = major;
    e[9] = uint8_t((ttl >> 16) & 0xFF);
    e[10] = uint8_t((ttl >> 8) & 0xFF);
    e[11] = uint8_t(ttl & 0xFF);
    if (eventgroup_id != 0xFFFF) {
        e[12] = uint8_t(eventgroup_id >> 8);
        e[13] = uint8_t(eventgroup_id);
        e[14] = 0;
        e[15] = 0;
    } else {
        e[12] = uint8_t(minor >> 24);
        e[13] = uint8_t(minor >> 16);
        e[14] = uint8_t(minor >> 8);
        e[15] = uint8_t(minor);
    }
    return e;
}

// Option wire: length(2) type(1) reserved(1).
struct SdOption {
    uint16_t length = 0;
    uint16_t type = 0;  // stored as a byte on the wire
    std::vector<uint8_t> value;
    std::vector<uint8_t> wire() const {
        std::vector<uint8_t> out;
        out.push_back(uint8_t(length >> 8));
        out.push_back(uint8_t(length));
        out.push_back(uint8_t(type));  // 1 byte on the wire
        out.push_back(0);              // reserved (1 byte)
        out.insert(out.end(), value.begin(), value.end());
        return out;
    }
};

struct SdEntry {
    uint8_t type = 0;
    uint8_t index_first = 0;
    uint8_t index_second = 0;
    uint8_t opts1 = 0;
    uint8_t opts2 = 0;
    uint16_t service_id = 0;
    uint16_t instance_id = 0;
    uint8_t major_version = 0;
    uint32_t ttl = 0;
    uint32_t minor_version = 0;
    uint16_t eventgroup_id = 0xFFFF;  // 0xFFFF sentinel -> none
    std::vector<SdOption> options;
};

inline SdEntry parse_entry(const uint8_t *p, size_t n) {
    if (n < ENTRY_SIZE) {
        throw SdError("short SD entry");
    }
    SdEntry e;
    e.type = p[0];
    e.index_first = p[1];
    e.index_second = p[2];
    e.opts1 = p[3] & 0x0F;
    e.opts2 = (p[3] >> 4) & 0x0F;
    e.service_id = uint16_t((p[4] << 8) | p[5]);
    e.instance_id = uint16_t((p[6] << 8) | p[7]);
    e.major_version = p[8];
    e.ttl = (uint32_t(p[9]) << 16) | (uint32_t(p[10]) << 8) | uint32_t(p[11]);
    if (e.type == SdEntryType::SUBSCRIBE || e.type == SdEntryType::SUBSCRIBE_ACK ||
        e.type == SdEntryType::SUBSCRIBE_NACK) {
        e.eventgroup_id = uint16_t((p[12] << 8) | p[13]);
    } else {
        e.minor_version = (uint32_t(p[12]) << 24) | (uint32_t(p[13]) << 16) |
                          (uint32_t(p[14]) << 8) | uint32_t(p[15]);
    }
    return e;
}

inline std::vector<uint8_t> encode_option_ipv4(
    const char *address, uint16_t port, uint16_t option_type = SdOptionType::IPV4_ENDPOINT,
    uint8_t protocol = 0x11) {
    std::vector<uint8_t> value(7);
    in_addr a;
    if (::inet_aton(address, &a) == 0) {
        throw SdError("invalid ipv4 in SD option: " + std::string(address));
    }
    value[0] = uint8_t(a.s_addr);
    value[1] = uint8_t(a.s_addr >> 8);
    value[2] = uint8_t(a.s_addr >> 16);
    value[3] = uint8_t(a.s_addr >> 24);
    value[4] = protocol;
    value[5] = uint8_t(port >> 8);
    value[6] = uint8_t(port);
    SdOption o;
    o.length = uint16_t(2 + value.size());
    o.type = option_type;
    o.value = std::move(value);
    return o.wire();
}

inline std::vector<SdOption> parse_options(const std::vector<uint8_t> &raw) {
    std::vector<SdOption> out;
    size_t off = 0;
    while (off + 4 <= raw.size()) {
        SdOption o;
        o.length = uint16_t((raw[off] << 8) | raw[off + 1]);
        o.type = uint16_t(raw[off + 2]);  // 1 byte on the wire
        if (o.length < 2 || off + 2 + o.length > raw.size()) {
            throw SdError("corrupt SD option");
        }
        o.value.assign(raw.begin() + off + 4, raw.begin() + off + 2 + o.length);
        out.push_back(std::move(o));
        off += 2 + o.length;
    }
    return out;
}

inline std::vector<SdEntry> parse_sd_message(const Message &msg) {
    const auto &p = msg.payload;
    if (p.size() < 8) {
        throw SdError("SD payload too short");
    }
    const uint32_t entries_len = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                 (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    if (4 + entries_len + 4 > p.size()) {
        throw SdError("SD entries length overruns payload");
    }
    std::vector<SdEntry> entries;
    size_t base = 4;
    for (size_t off = base; off + ENTRY_SIZE <= base + entries_len; off += ENTRY_SIZE) {
        entries.push_back(parse_entry(&p[off], ENTRY_SIZE));
    }
    const size_t opts_base = base + entries_len;
    const uint32_t opts_len = (uint32_t(p[opts_base]) << 24) |
                              (uint32_t(p[opts_base + 1]) << 16) |
                              (uint32_t(p[opts_base + 2]) << 8) |
                              uint32_t(p[opts_base + 3]);
    if (opts_base + 4 + opts_len > p.size()) {
        throw SdError("SD options length overruns payload");
    }
    std::vector<SdOption> options;
    size_t off = opts_base + 4;
    size_t opts_end = off + opts_len;
    while (off + 4 <= opts_end) {
        SdOption o;
        o.length = uint16_t((p[off] << 8) | p[off + 1]);
        o.type = uint16_t(p[off + 2]);  // 1 byte on the wire
        if (o.length < 2 || off + 2 + o.length > opts_end) {
            throw SdError("corrupt SD option");
        }
        o.value.assign(p.begin() + off + 4, p.begin() + off + 2 + o.length);
        options.push_back(std::move(o));
        off += 2 + o.length;
    }
    // attach options referenced by each entry
    for (auto &e : entries) {
        std::vector<size_t> idx;
        if (e.opts1) {
            for (uint8_t k = 0; k < e.opts1; ++k) {
                idx.push_back(e.index_first + k);
            }
        }
        if (e.opts2) {
            for (uint8_t k = 0; k < e.opts2; ++k) {
                idx.push_back(e.index_second + k);
            }
        }
        for (size_t i : idx) {
            if (i < options.size()) {
                e.options.push_back(options[i]);
            }
        }
    }
    return entries;
}

inline Message build_sd_message(const std::vector<std::vector<uint8_t>> &entries,
                                const std::vector<std::vector<uint8_t>> &options,
                                uint16_t client_id = 0, uint16_t session_id = 1) {
    std::vector<uint8_t> payload;
    size_t e_len = 0, o_len = 0;
    for (const auto &e : entries) {
        e_len += e.size();
    }
    for (const auto &o : options) {
        o_len += o.size();
    }
    const size_t pad_e = (4 - (e_len % 4)) % 4;
    const size_t pad_o = (4 - (o_len % 4)) % 4;
    payload.push_back(uint8_t(entries.empty() ? 0 : (e_len >> 24)));
    payload.push_back(uint8_t(e_len >> 16));
    payload.push_back(uint8_t(e_len >> 8));
    payload.push_back(uint8_t(e_len));
    for (const auto &e : entries) {
        payload.insert(payload.end(), e.begin(), e.end());
    }
    payload.insert(payload.end(), pad_e, 0);
    payload.push_back(uint8_t(o_len >> 24));
    payload.push_back(uint8_t(o_len >> 16));
    payload.push_back(uint8_t(o_len >> 8));
    payload.push_back(uint8_t(o_len));
    for (const auto &o : options) {
        payload.insert(payload.end(), o.begin(), o.end());
    }
    payload.insert(payload.end(), pad_o, 0);

    Message m;
    m.header.service_id = SD_SERVICE_ID;
    m.header.method_id = SD_METHOD_ID;
    m.header.client_id = client_id;
    m.header.session_id = session_id;
    m.header.interface_version = SD_INTERFACE_VERSION;
    m.header.protocol_version = PROTOCOL_VERSION;
    m.header.message_type = MessageType::NOTIFICATION;
    m.payload = std::move(payload);
    return m;
}

inline std::pair<std::string, uint16_t> ipv4_endpoint(const SdOption &opt) {
    if (opt.type != SdOptionType::IPV4_ENDPOINT && opt.type != SdOptionType::IPV4_MULTICAST) {
        return {"", 0};
    }
    if (opt.value.size() < 7) {
        return {"", 0};
    }
    const auto &v = opt.value;
    // inet_aton/inet_ntoa use the local byte order representation of s_addr;
    // re-encode as a binary address is unnecessary, write a dotted string.
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    return {buf, uint16_t((v[5] << 8) | v[6])};
}

// ------------------------------------------------------- service publisher ----

struct OfferedService {
    uint16_t service_id = 0;
    uint16_t instance_id = 0;
    uint8_t major = 0;
    uint32_t minor = 0;
    std::string address = "0.0.0.0";
    uint16_t port = 0;
    std::set<uint16_t> eventgroups;
};

// Subscriber identity = (host, port) datagram endpoint, taken from the
// SUBSCRIBE IPv4-endpoint option (datagram source as fallback).
struct Subscriber {
    std::string host;
    uint16_t port = 0;
    bool operator<(const Subscriber &o) const {
        return host != o.host ? host < o.host : port < o.port;
    }
    bool operator==(const Subscriber &o) const {
        return host == o.host && port == o.port;
    }
};

class ServicePublisher {
public:
    using Clock = std::function<double()>;

    explicit ServicePublisher(std::vector<OfferedService> offers = {},
                              Clock clock = default_now)
        : clock_(clock) {
        for (auto &o : offers) {
            add_service(std::move(o));
        }
    }

    void add_service(OfferedService o) {
        ServiceDesc desc;
        desc.offered = std::move(o);
        desc.phase = "retry";
        desc.step = 0;
        desc.next = clock_() + uniform(0.0, 0.1);
        services_[{desc.offered.service_id, desc.offered.instance_id}] = std::move(desc);
    }

    // Emit due OFFER messages (exponential backoff then periodic cycle).
    std::vector<Message> process(double now) {
        std::vector<Message> out;
        for (auto &kv : services_) {
            ServiceDesc &d = kv.second;
            if (now < d.next) {
                continue;
            }
            out.push_back(offer_message(d.offered, false));
            if (d.step < SOMEIP_SD_REPETITIONS_MAX) {
                d.next = now + SOMEIP_SD_REPETITIONS_BASE_DELAY * (1 << d.step);
                ++d.step;
            } else {
                d.next = now + SOMEIP_SD_CYCLE;
            }
        }
        return out;
    }

    void handle_datagram(const Message &msg, const std::string &src_host,
                         uint16_t src_port) {
        if (msg.header.service_id != SD_SERVICE_ID) {
            return;
        }
        // sawtooth guard: ignore sessions older than the last seen one
        const uint16_t sid = msg.header.session_id;
        auto it = last_session_.find(src_host);
        if (it != last_session_.end()) {
            if (uint16_t(sid - it->second) > 0x7FFF) {
                return;
            }
        }
        last_session_[src_host] = sid;
        std::vector<SdEntry> entries;
        try {
            entries = parse_sd_message(msg);
        } catch (const SdError &) {
            return;
        }
        for (auto &e : entries) {
            auto sit = services_.find({e.service_id, e.instance_id});
            if (sit == services_.end()) {
                continue;
            }
            OfferedService &o = sit->second.offered;
            switch (e.type) {
                case SdEntryType::FIND:
                    pending_.push_back(offer_message(o, false));
                    break;
                case SdEntryType::SUBSCRIBE: {
                    const bool known = o.eventgroups.count(e.eventgroup_id) != 0;
                    if (known) {
                        Subscriber target;
                        bool have = false;
                        for (const auto &opt : e.options) {
                            if (opt.type == SdOptionType::IPV4_ENDPOINT) {
                                auto ep = ipv4_endpoint(opt);
                                if (!ep.first.empty()) {
                                    target.host = ep.first;
                                    target.port = ep.second;
                                    have = true;
                                    break;
                                }
                            }
                        }
                        if (!have) {  // fall back to the datagram source
                            target.host = src_host;
                            target.port = src_port;
                        }
                        subscribers_[{o.service_id, o.instance_id}].insert(std::move(target));
                    }
                    std::vector<std::vector<uint8_t>> entries_b = {
                        encode_entry(known ? SdEntryType::SUBSCRIBE_ACK
                                           : SdEntryType::SUBSCRIBE_NACK,
                                     o.service_id, o.instance_id, o.major,
                                     SOMEIP_SD_DEFAULT_TTL,
                                     0 /*minor*/, e.eventgroup_id)};
                    pending_.push_back(build_sd_message(entries_b, {}, 0, 1));
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Drain responses produced by the last handle_datagram().
    std::vector<Message> drain_pending() {
        std::vector<Message> out = std::move(pending_);
        pending_.clear();
        return out;
    }

    const std::set<Subscriber> &subscribers(uint16_t svc, uint16_t inst) {
        static const std::set<Subscriber> empty;
        auto it = subscribers_.find({svc, inst});
        return it == subscribers_.end() ? empty : it->second;
    }

    std::vector<Message> stop() {
        std::vector<Message> out;
        for (auto &kv : services_) {
            out.push_back(offer_message(kv.second.offered, true));
        }
        return out;
    }

    static double default_now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count() /
               1000.0;
    }

private:
    struct ServiceDesc {
        OfferedService offered;
        std::string phase;
        int step = 0;
        double next = 0.0;
    };
    using Key = std::pair<uint16_t, uint16_t>;

    Message offer_message(const OfferedService &o, bool stop) {
        const uint32_t ttl = stop ? 0 : SOMEIP_SD_DEFAULT_TTL;
        std::vector<std::vector<uint8_t>> entries = {
            encode_entry(SdEntryType::OFFER, o.service_id, o.instance_id,
                         o.major, ttl, o.minor, 0xFFFF, 0, 0, stop ? 0 : 1)};
        std::vector<std::vector<uint8_t>> options;
        if (!stop) {
            options.push_back(encode_option_ipv4(o.address.c_str(), o.port));
        }
        return build_sd_message(entries, options, 0, 1);
    }

    double uniform(double lo, double hi) {
        if (hi <= lo) {
            return lo;
        }
        std::uniform_real_distribution<double> d(lo, hi);
        return d(rng_);
    }

    Clock clock_;
    std::map<Key, ServiceDesc> services_;
    std::map<Key, std::set<Subscriber>> subscribers_;
    std::map<std::string, uint16_t> last_session_;
    std::vector<Message> pending_;
    std::mt19937 rng_{std::random_device{}()};
};

// ------------------------------------------------------- client monitor ----

class ServiceMonitor {
public:
    using Clock = std::function<double()>;
    using Key = std::pair<uint16_t, uint16_t>;
    using Callback = std::function<void(uint16_t, uint16_t)>;
    using GroupCallback = std::function<void(uint16_t, uint16_t, uint16_t)>;

    struct Offer {
        Key key;
        uint8_t major = 0;
        uint32_t minor = 0;
        std::string address;
        uint16_t port = 0;
        double deadline = 0.0;
        bool available = true;
    };

    explicit ServiceMonitor(uint16_t client_id = 0x0000, Clock clock = ServicePublisher::default_now)
        : client_id_(client_id), clock_(clock) {}

    void find(uint16_t service_id, uint16_t instance_id,
              std::set<uint16_t> eventgroups = {}) {
        Key key{service_id, instance_id};
        Target t;
        t.eventgroups = std::move(eventgroups);
        t.step = 0;
        t.next = clock_() + uniform(0.0, 0.1);
        wanted_[key] = std::move(t);
    }
    void set_client_endpoint(const std::string &address, uint16_t port) {
        client_address_ = address;
        client_port_ = port;
    }

    void on_available(Callback cb) { on_available_ = std::move(cb); }
    void on_unavailable(Callback cb) { on_unavailable_ = std::move(cb); }
    void on_subscribe_ok(GroupCallback cb) { on_subscribe_ok_ = std::move(cb); }
    void on_subscribe_nack(GroupCallback cb) { on_subscribe_nack_ = std::move(cb); }

    const Offer *offer(Key key) const {
        auto it = offers_.find(key);
        return it == offers_.end() ? nullptr : &it->second;
    }

    std::set<uint16_t> wanted_eventgroups(uint16_t service_id,
                                          uint16_t instance_id) const {
        auto it = wanted_.find({service_id, instance_id});
        return it == wanted_.end() ? std::set<uint16_t>{} : it->second.eventgroups;
    }

    enum class SubState { None, Ack, Nack };

    SubState subscription_state(uint16_t svc, uint16_t inst,
                                uint16_t eventgroup_id) const {
        const Key k{svc, inst};
        if (acks_.count({svc, inst, eventgroup_id})) {
            return SubState::Ack;
        }
        if (nacks_.count({svc, inst, eventgroup_id})) {
            return SubState::Nack;
        }
        return SubState::None;
    }

    // (Re)emit SUBSCRIBE when the service is already known and available.
    void resubscribe(uint16_t svc, uint16_t inst) {
        const Key key{svc, inst};
        auto oit = offers_.find(key);
        if (oit == offers_.end() || !oit->second.available) {
            return;
        }
        auto t = wanted_.find(key);
        if (t == wanted_.end() || t->second.eventgroups.empty()) {
            return;
        }
        if (subscribed_.find(key) == subscribed_.end()) {
            subscribed_[key] = Subscription{clock_() + SOMEIP_SD_DEFAULT_TTL};
            pending_.push_back(subscribe_message(key));
        }
    }

    // Emit due FIND messages; stop advertising once a service is available.
    std::vector<Message> process(double now) {
        std::vector<Message> out;
        for (auto &kv : wanted_) {
            const Key &key = kv.first;
            auto oit = offers_.find(key);
            if (oit != offers_.end() && oit->second.available) {
                continue;
            }
            Target &t = kv.second;
            if (now < t.next) {
                continue;
            }
            out.push_back(find_message(key));
            if (t.step < SOMEIP_SD_REPETITIONS_MAX) {
                t.next = now + SOMEIP_SD_REQUESTS_BASE_DELAY * (1 << t.step);
                ++t.step;
            } else {
                t.next = now + SOMEIP_SD_CYCLE;
            }
        }
        // subscription renewal
        for (auto &kv : subscribed_) {
            if (now >= kv.second.renew_at) {
                out.push_back(subscribe_message(kv.first));
                kv.second.renew_at = now + SOMEIP_SD_DEFAULT_TTL;
            }
        }
        out.insert(out.end(), pending_.begin(), pending_.end());
        pending_.clear();
        return out;
    }

    // Subscribe messages emitted immediately upon an offer (before the first
    // renewal window): drained here or via process().
    std::vector<Message> drain_pending() {
        std::vector<Message> out = std::move(pending_);
        pending_.clear();
        return out;
    }

    void handle_datagram(const Message &msg, const std::string & /*src*/) {
        if (msg.header.service_id != SD_SERVICE_ID) {
            return;
        }
        std::vector<SdEntry> entries;
        try {
            entries = parse_sd_message(msg);
        } catch (const SdError &) {
            return;
        }
        for (auto &e : entries) {
            Key key{e.service_id, e.instance_id};
            switch (e.type) {
                case SdEntryType::OFFER:
                    on_offer(key, e);
                    break;
                case SdEntryType::SUBSCRIBE_ACK: {
                    acks_.insert({key.first, key.second, e.eventgroup_id});
                    nacks_.erase({key.first, key.second, e.eventgroup_id});
                    auto it = subscribed_.find(key);
                    if (it != subscribed_.end()) {
                        it->second.renew_at = clock_() + SOMEIP_SD_DEFAULT_TTL;
                    }
                    if (on_subscribe_ok_) {
                        on_subscribe_ok_(key.first, key.second, e.eventgroup_id);
                    }
                    break;
                }
                case SdEntryType::SUBSCRIBE_NACK:
                    acks_.erase({key.first, key.second, e.eventgroup_id});
                    nacks_.insert({key.first, key.second, e.eventgroup_id});
                    subscribed_.erase(key);
                    if (on_subscribe_nack_) {
                        on_subscribe_nack_(key.first, key.second, e.eventgroup_id);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    void tick(double now) {
        for (auto it = offers_.begin(); it != offers_.end();) {
            Offer &o = it->second;
            if (o.available && now >= o.deadline) {
                o.available = false;
                if (on_unavailable_) {
                    on_unavailable_(it->first.first, it->first.second);
                }
            }
            ++it;
        }
    }

private:
    struct Target {
        std::set<uint16_t> eventgroups;
        int step = 0;
        double next = 0.0;
    };
    struct Subscription {
        double renew_at = 0.0;
    };

    Message find_message(const Key &key) {
        std::vector<std::vector<uint8_t>> entries = {
            encode_entry(SdEntryType::FIND, key.first, key.second, 0x01,
                         SOMEIP_SD_DEFAULT_TTL, 0)};
        return build_sd_message(entries, {}, client_id_, next_session());
    }

    Message subscribe_message(const Key &key) {
        auto oit = offers_.find(key);
        const Offer *o = oit == offers_.end() ? nullptr : &oit->second;
        const auto t = wanted_.find(key);
        if (!o || t == wanted_.end()) {
            return build_sd_message({}, {}, client_id_, next_session());
        }
        std::vector<std::vector<uint8_t>> entries;
        std::vector<std::vector<uint8_t>> options = {
            encode_option_ipv4(client_address_.c_str(), client_port_)};
        for (uint16_t gid : t->second.eventgroups) {
            entries.push_back(encode_entry(SdEntryType::SUBSCRIBE, key.first,
                                           key.second, o->major,
                                           SOMEIP_SD_DEFAULT_TTL, 0, gid, 0, 0, 1));
        }
        return build_sd_message(entries, options, client_id_, next_session());
    }

    void on_offer(const Key &key, const SdEntry &e) {
        if (e.ttl == 0) {  // StopOffer
            auto it = offers_.find(key);
            if (it != offers_.end() && it->second.available) {
                if (on_unavailable_) {
                    on_unavailable_(key.first, key.second);
                }
            }
            offers_.erase(key);
            return;
        }
        std::string addr = "0.0.0.0";
        uint16_t port = 0;
        for (const auto &opt : e.options) {
            auto ep = ipv4_endpoint(opt);
            if (!ep.first.empty()) {
                addr = ep.first;
                port = ep.second;
                break;
            }
        }
        const auto it = offers_.find(key);
        const bool was_available = it != offers_.end() && it->second.available;
        Offer o;
        o.key = key;
        o.major = e.major_version;
        o.minor = e.minor_version;
        o.address = addr;
        o.port = port;
        o.deadline = clock_() + e.ttl;
        o.available = true;
        offers_[key] = o;
        if (!was_available) {
            const auto t = wanted_.find(key);
            if (t != wanted_.end() && !t->second.eventgroups.empty()) {
                subscribed_[key] = Subscription{clock_() + SOMEIP_SD_DEFAULT_TTL};
                // emit the first SUBSCRIBE immediately upon an offer
                pending_.push_back(subscribe_message(key));
            }
            if (on_available_) {
                on_available_(key.first, key.second);
            }
        }
    }

    uint16_t next_session() { return session_++; }
    double uniform(double lo, double hi) {
        if (hi <= lo) {
            return lo;
        }
        std::uniform_real_distribution<double> d(lo, hi);
        return d(rng_);
    }

    uint16_t client_id_ = 0;
    Clock clock_;
    std::string client_address_ = "0.0.0.0";
    uint16_t client_port_ = 0;
    std::map<Key, Target> wanted_;
    std::map<Key, Offer> offers_;
    std::map<Key, Subscription> subscribed_;
    std::set<std::tuple<uint16_t, uint16_t, uint16_t>> acks_;
    std::set<std::tuple<uint16_t, uint16_t, uint16_t>> nacks_;
    std::vector<Message> pending_;
    Callback on_available_;
    Callback on_unavailable_;
    GroupCallback on_subscribe_ok_;
    GroupCallback on_subscribe_nack_;
    uint16_t session_ = 1;
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace sdm
} // namespace someip

#endif // SOMEIP_SDM_HPP
#ifndef SOMEIP_APP_HPP
#define SOMEIP_APP_HPP

// High-level application API: threaded service and client.
// Mirrors platform/python/someip/app.py (C++17, POSIX, no third-party deps).
//
// - SomeipServiceV2: wire the offline ServicePublisher + datagram endpoints
//   + SOME/IP-TP into a ready-to-use server (method loop + SD loop threads).
// - ClientV2: one UDP socket for requests + notifications, one SD multicast
//   socket, a single client loop thread driving discovery/subscriptions.
// - local_ip(): best-effort default-route IPv4 (loopback fallback).
//
// Threading notes: each endpoint's sockets are owned by their loop thread;
// callbacks run on the owning thread. Public mutations go through
// thread-safe helpers. stop() is idempotent.

#include "sdm.hpp"
#include "transport.hpp"
#include "tpc.hpp"
#include "wire.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace someip {
namespace app {

using sdm::SD_MULTICAST_ADDRESS;
using sdm::SD_PORT;

struct AppError : std::runtime_error {
    explicit AppError(const std::string &what) : std::runtime_error(what) {}
};
struct NotFound : AppError {
    explicit NotFound(const std::string &what) : AppError(what) {}
};
struct TimeoutError : AppError {
    explicit TimeoutError(const std::string &what) : AppError(what) {}
};

// Hard cap on in-flight requests per client (backpressure guard).
constexpr size_t MAX_IN_FLIGHT_REQUESTS = 1024;

// ------------------------------------------------------------------ utils --

inline std::string local_ip() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return "127.0.0.1";
    }
    sockaddr_in dst;
    std::memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(80);
    dst.sin_addr.s_addr = ::inet_addr("8.8.8.8");
    char host[INET_ADDRSTRLEN] = {0};
    bool ok = false;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&dst), sizeof(dst)) == 0) {
        sockaddr_in me;
        socklen_t len = sizeof(me);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&me), &len) == 0) {
            if (::inet_ntop(AF_INET, &me.sin_addr, host, sizeof(host)) != nullptr) {
                ok = true;
            }
        }
    }
    ::close(fd);
    return ok ? std::string(host) : "127.0.0.1";
}

inline std::vector<uint8_t> be_u32(uint32_t v) {
    return std::vector<uint8_t>{
        uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
}

inline uint32_t unpack_u32(const std::vector<uint8_t> &p, size_t off = 0) {
    if (p.size() < off + 4) {
        throw AppError("short uint32 field");
    }
    return (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) |
           (uint32_t(p[off + 2]) << 8) | uint32_t(p[off + 3]);
}

// Send msg, transparently applying SOME/IP-TP framing. Throws on transport
// errors (UdpEndpoint::send).
inline void send_message(UdpEndpoint &ep, const Message &msg,
                         const sockaddr_in &dst) {
    for (const auto &frame : someip::segment(msg)) {
        std::vector<uint8_t> raw;
        frame.to_bytes(raw);
        ep.send(raw.data(), raw.size(), dst);
    }
}

inline std::string host_from(const sockaddr_in &src) {
    char host[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &src.sin_addr, host, sizeof(host)) == nullptr) {
        return "0.0.0.0";
    }
    return std::string(host);
}

inline sockaddr_in sd_destination(uint16_t sd_port) {
    return UdpEndpoint::ipv4(SD_MULTICAST_ADDRESS, sd_port);
}

class ReassemblePass {
public:
    // Feed one wire Message; returns the completed Message for non-TP frames
    // and for the final TP segment, std::nullopt meanwhile (passthrough=true
    // mirrors the Python layer).
    std::optional<Message> feed(const Message &m) {
        try {
            return reasm_.add(m);
        } catch (const someip::TpError &) {
            return std::nullopt;
        }
    }
    void tick() { reasm_.tick(); }

private:
    someip::Reassembler reasm_;
};

// ============================================================== service ====

using Handler = std::function<std::pair<uint8_t, std::vector<uint8_t>>(
    const std::vector<uint8_t> &, const sockaddr_in &)>;

class SomeipServiceV2 {
public:
    SomeipServiceV2(uint16_t service_id, uint16_t instance_id,
                    uint8_t major_version = 0x01, uint32_t minor_version = 1,
                    uint16_t method_port = 30500, uint16_t event_port = 30501,
                    uint16_t sd_port = SD_PORT, std::string interface_ip = "",
                    uint16_t client_id = 0)
        : service_id_(service_id),
          instance_id_(instance_id),
          major_version_(major_version),
          minor_version_(minor_version),
          method_port_(method_port),
          event_port_(event_port),
          sd_port_(sd_port),
          interface_ip_(interface_ip.empty() ? local_ip()
                                             : std::move(interface_ip)),
          client_id_(client_id) {}

    // -- configuration ----------------------------------------------------
    void add_method(uint16_t method_id, Handler handler) {
        methods_[method_id] = std::move(handler);
    }

    void add_event(uint16_t event_id, uint16_t eventgroup_id) {
        events_[event_id] = eventgroup_id;
        eventgroups_[eventgroup_id].insert(event_id);
    }

    // Speed-style field: getter at field_id, setter at field_id+1 and a
    // notifier event at field_id+2 on eventgroup_id.
    void add_uint32_field(uint16_t field_id, uint16_t eventgroup_id,
                          uint32_t initial = 0) {
        {
            std::lock_guard<std::mutex> lk(field_mx_);
            fields_[field_id] = initial;
        }
        add_method(field_id, [this, field_id](const std::vector<uint8_t> &,
                                              const sockaddr_in &) {
            std::lock_guard<std::mutex> lk(field_mx_);
            const auto it = fields_.find(field_id);
            const uint32_t v = it == fields_.end() ? 0 : it->second;
            return std::make_pair(uint8_t(ReturnCode::E_OK), be_u32(v));
        });
        add_method(field_id + 1, [this, field_id](
                                     const std::vector<uint8_t> &p,
                                     const sockaddr_in &) {
            if (p.size() != 4) {
                return std::make_pair(uint8_t(ReturnCode::E_MALFORMED_MESSAGE),
                                      std::vector<uint8_t>{});
            }
            const uint32_t v = unpack_u32(p, 0);
            {
                std::lock_guard<std::mutex> lk(field_mx_);
                fields_[field_id] = v;
            }
            publish_event(field_id + 2, p);
            return std::make_pair(uint8_t(ReturnCode::E_OK), p);
        });
        add_event(field_id + 2, eventgroup_id);
    }

    void set_field(uint16_t field_id, uint32_t value) {
        const std::vector<uint8_t> payload = be_u32(value);
        {
            std::lock_guard<std::mutex> lk(field_mx_);
            fields_[field_id] = value;
        }
        publish_event(field_id + 2, payload);
    }

    // -- lifecycle --------------------------------------------------------
    const std::string &interface_ip() const { return interface_ip_; }
    uint16_t method_port() const { return method_port_; }
    uint16_t event_port() const { return event_port_; }
    uint16_t sd_port() const { return sd_port_; }

    void start() {
        method_ep_ = std::make_unique<UdpEndpoint>(
            UdpEndpoint::unicast(method_port_, "0.0.0.0", true));
        event_ep_ = std::make_unique<UdpEndpoint>(
            UdpEndpoint::unicast(event_port_, "0.0.0.0", true));
        sd_ep_ = std::make_unique<UdpEndpoint>(UdpEndpoint::multicast(
            sd_port_, SD_MULTICAST_ADDRESS, interface_ip_.c_str()));
        method_port_ = method_ep_->local_port();
        event_port_ = event_ep_->local_port();

        sdm::OfferedService o;
        o.service_id = service_id_;
        o.instance_id = instance_id_;
        o.major = major_version_;
        o.minor = minor_version_;
        o.address = interface_ip_;
        o.port = method_port_;
        for (const auto &kv : eventgroups_) {
            o.eventgroups.insert(kv.first);
        }
        pub_ = std::make_unique<sdm::ServicePublisher>(
            std::vector<sdm::OfferedService>{std::move(o)});

        t_method_ = std::thread(&SomeipServiceV2::method_loop, this);
        t_sd_ = std::thread(&SomeipServiceV2::sd_loop, this);
    }

    void stop() {
        stop_.store(true);
        if (pub_) {
            std::vector<Message> stop_msgs;
            {
                std::lock_guard<std::mutex> lk(sd_mx_);
                stop_msgs = pub_->stop();
            }
            for (const auto &m : stop_msgs) {
                send_sd(m);
            }
        }
        if (method_ep_) {
            method_ep_->close();
        }
        if (event_ep_) {
            event_ep_->close();
        }
        if (sd_ep_) {
            sd_ep_->close();
        }
        if (t_method_.joinable()) {
            t_method_.join();
        }
        if (t_sd_.joinable()) {
            t_sd_.join();
        }
    }

    // -- notifications ----------------------------------------------------
    void publish_event(uint16_t event_id, const std::vector<uint8_t> &payload) {
        if (events_.find(event_id) == events_.end() || !event_ep_ || !pub_) {
            return;
        }
        Message m;
        m.header.service_id = service_id_;
        m.header.method_id = event_id;
        m.header.client_id = client_id_;
        m.header.session_id = next_src_session();
        m.header.interface_version = major_version_;
        m.header.message_type = MessageType::NOTIFICATION;
        m.header.return_code = 0;
        m.payload = payload;

        std::vector<sdm::Subscriber> targets;
        {
            std::lock_guard<std::mutex> lk(sd_mx_);
            const auto &s = pub_->subscribers(service_id_, instance_id_);
            targets.assign(s.begin(), s.end());
        }
        for (const auto &t : targets) {
            try {
                send_message(*event_ep_, m,
                             UdpEndpoint::ipv4(t.host.c_str(), t.port));
            } catch (const TransportError &) {
            }
        }
    }

private:
    uint16_t next_src_session() {
        std::lock_guard<std::mutex> lk(session_mx_);
        session_ = (session_ + 1) & 0xFFFF;
        return session_;
    }

    void send_sd(const Message &m) {
        if (!sd_ep_) {
            return;
        }
        std::vector<uint8_t> raw;
        m.to_bytes(raw);
        try {
            sd_ep_->send(raw.data(), raw.size(), sd_destination(sd_port_));
        } catch (const TransportError &) {
        }
    }

    void method_loop() {
        ReassemblePass recon;
        std::vector<uint8_t> raw;
        sockaddr_in src;
        while (!stop_.load()) {
            if (method_ep_ && method_ep_->recv(raw, src, 200)) {
                try {
                    const Message m = Message::from_bytes(raw.data(), raw.size());
                    if (auto full = recon.feed(m)) {
                        handle_request(*full, src);
                    }
                } catch (const WireError &) {
                }
            }
            recon.tick();
        }
    }

    void handle_request(const Message &msg, const sockaddr_in &src) {
        const Header &h = msg.header;
        if (h.service_id != service_id_) {
            return;
        }
        if (h.message_type != MessageType::REQUEST &&
            h.message_type != MessageType::REQUEST_NO_RETURN) {
            return;
        }
        uint8_t rc = ReturnCode::E_UNKNOWN_METHOD;
        std::vector<uint8_t> resp;
        const auto it = methods_.find(h.method_id);
        if (it != methods_.end()) {
            try {
                std::tie(rc, resp) = it->second(msg.payload, src);
            } catch (...) {
                rc = ReturnCode::E_NOT_OK;
                resp.clear();
            }
        }
        if (h.message_type == MessageType::REQUEST) {
            Message out;
            out.header.service_id = service_id_;
            out.header.method_id = h.method_id;
            out.header.client_id = h.client_id;
            out.header.session_id = h.session_id;
            out.header.interface_version = major_version_;
            out.header.message_type = MessageType::RESPONSE;
            out.header.return_code = rc;
            out.payload = std::move(resp);
            try {
                send_message(*method_ep_, out, src);
            } catch (const TransportError &) {
            }
        }
    }

    void sd_loop() {
        std::vector<uint8_t> raw;
        sockaddr_in src;
        while (!stop_.load()) {
            if (sd_ep_ && sd_ep_->recv(raw, src, 100)) {
                try {
                    const Message m = Message::from_bytes(raw.data(), raw.size());
                    std::lock_guard<std::mutex> lk(sd_mx_);
                    pub_->handle_datagram(m, host_from(src),
                                          ntohs(src.sin_port));
                    for (const auto &out : pub_->drain_pending()) {
                        send_sd(out);
                    }
                } catch (const WireError &) {
                }
            }
            std::vector<Message> due;
            {
                std::lock_guard<std::mutex> lk(sd_mx_);
                due = pub_->process(sdm::ServicePublisher::default_now());
            }
            for (const auto &m : due) {
                send_sd(m);
            }
        }
    }

    uint16_t service_id_;
    uint16_t instance_id_;
    uint8_t major_version_;
    uint32_t minor_version_;
    uint16_t method_port_;
    uint16_t event_port_;
    uint16_t sd_port_;
    std::string interface_ip_;
    uint16_t client_id_;

    std::map<uint16_t, Handler> methods_;
    std::map<uint16_t, uint16_t> events_;         // event_id -> eventgroup
    std::map<uint16_t, std::set<uint16_t>> eventgroups_;  // egid -> events
    std::map<uint16_t, uint32_t> fields_;
    std::mutex field_mx_;

    std::unique_ptr<sdm::ServicePublisher> pub_;
    std::mutex sd_mx_;
    std::unique_ptr<UdpEndpoint> method_ep_;
    std::unique_ptr<UdpEndpoint> event_ep_;
    std::unique_ptr<UdpEndpoint> sd_ep_;

    std::atomic<bool> stop_{false};
    std::mutex session_mx_;
    uint16_t session_ = 0;
    std::thread t_method_;
    std::thread t_sd_;
};

// ============================================================== client ====

using EventFn = std::function<void(uint16_t, const std::vector<uint8_t> &)>;
using AvailFn = std::function<void(uint16_t, uint16_t)>;
using GroupFn = std::function<void(uint16_t, uint16_t, uint16_t)>;

class ClientV2 {
public:
    ClientV2(uint16_t client_id = 0, uint16_t sd_port = SD_PORT,
             std::string interface_ip = "")
        : client_id_(client_id != 0 ? client_id : random_client_id()),
          sd_port_(sd_port),
          interface_ip_(interface_ip.empty() ? local_ip()
                                             : std::move(interface_ip)),
          monitor_(client_id_) {}

    // -- configuration ----------------------------------------------------
    void on_event(uint16_t event_id, EventFn fn) {
        std::lock_guard<std::mutex> lk(mx_);
        event_cb_[event_id] = std::move(fn);
    }
    void on_available(AvailFn fn) {
        std::lock_guard<std::mutex> lk(mx_);
        avail_.push_back(std::move(fn));
    }
    void on_unavailable(AvailFn fn) {
        std::lock_guard<std::mutex> lk(mx_);
        unavail_.push_back(std::move(fn));
    }
    void on_subscribe_ok(GroupFn fn) {
        std::lock_guard<std::mutex> lk(mx_);
        sub_ok_ = std::move(fn);
    }
    void on_subscribe_nack(GroupFn fn) {
        std::lock_guard<std::mutex> lk(mx_);
        sub_nack_ = std::move(fn);
    }

    // -- lifecycle --------------------------------------------------------
    void start() {
        endpoint_ = std::make_unique<UdpEndpoint>(UdpEndpoint::unicast(0));
        sd_ep_ = std::make_unique<UdpEndpoint>(UdpEndpoint::multicast(
            sd_port_, SD_MULTICAST_ADDRESS, interface_ip_.c_str()));
        monitor_.set_client_endpoint(interface_ip_, endpoint_->local_port());
        monitor_.on_available([this](uint16_t s, uint16_t i) {
            events_.push_back(Event{EventKind::Available, s, i, 0, {}});
            cv_.notify_all();
        });
        monitor_.on_unavailable([this](uint16_t s, uint16_t i) {
            events_.push_back(Event{EventKind::Unavailable, s, i, 0, {}});
            cv_.notify_all();
        });
        monitor_.on_subscribe_ok([this](uint16_t s, uint16_t i, uint16_t g) {
            events_.push_back(Event{EventKind::SubOk, s, i, g, {}});
            cv_.notify_all();
        });
        monitor_.on_subscribe_nack([this](uint16_t s, uint16_t i, uint16_t g) {
            events_.push_back(Event{EventKind::SubNack, s, i, g, {}});
            cv_.notify_all();
        });
        thread_ = std::thread(&ClientV2::client_loop, this);
    }

    void stop() {
        stop_.store(true);
        if (endpoint_) {
            endpoint_->close();
        }
        if (sd_ep_) {
            sd_ep_->close();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // -- high-level ops ---------------------------------------------------
    bool wait_for_service(uint16_t service_id, uint16_t instance_id,
                          std::set<uint16_t> eventgroups = {},
                          double timeout = 10.0) {
        {
            std::lock_guard<std::mutex> lk(mx_);
            monitor_.find(service_id, instance_id, std::move(eventgroups));
        }
        std::unique_lock<std::mutex> lk(mx_);
        const sdm::ServiceMonitor::Key key{service_id, instance_id};
        return cv_.wait_for(lk, secs(timeout), [&] {
            const auto *o = monitor_.offer(key);
            return o != nullptr && o->available;
        });
    }

    std::optional<std::pair<std::string, uint16_t>> discovered_endpoint(
        uint16_t service_id, uint16_t instance_id) {
        std::lock_guard<std::mutex> lk(mx_);
        const auto *o = monitor_.offer({service_id, instance_id});
        if (o == nullptr || !o->available) {
            return std::nullopt;
        }
        return std::make_pair(o->address, o->port);
    }

    std::pair<uint8_t, std::vector<uint8_t>> request(
        uint16_t service_id, uint16_t instance_id, uint16_t method_id,
        const std::vector<uint8_t> &payload = {}, double timeout = 3.0) {
        sockaddr_in dst;
        uint16_t session;
        {
            std::lock_guard<std::mutex> lk(mx_);
            const auto *o = monitor_.offer({service_id, instance_id});
            if (o == nullptr || !o->available) {
                throw NotFound("service 0x" + hex4(service_id) + "/0x" +
                               hex4(instance_id) + " not discovered");
            }
            dst = UdpEndpoint::ipv4(o->address.c_str(), o->port);
            session = next_client_session();
            if (pending_.size() >= MAX_IN_FLIGHT_REQUESTS) {
                throw AppError("in-flight request limit reached (backpressure)");
            }
            pending_[session] = Pending{service_id, now_plus(timeout), false, false,
                                std::nullopt};
        }
        Message m;
        m.header.service_id = service_id;
        m.header.method_id = method_id;
        m.header.client_id = client_id_;
        m.header.session_id = session;
        m.header.interface_version = 0x01;
        m.header.message_type = MessageType::REQUEST;
        m.payload = payload;
        send_message(*endpoint_, m, dst);  // throws TransportError

        std::unique_lock<std::mutex> lk(mx_);
        auto it = pending_.find(session);
        if (it == pending_.end()) {
            throw TimeoutError("SOME/IP request 0x" + hex4(method_id) +
                               " timed out");
        }
        const bool ready =
            cv_.wait_until(lk, it->second.deadline, [&] { return it->second.ready; });
        auto res = it->second.result;
        const bool stale = it->second.stale;
        pending_.erase(session);
        if (ready && !stale && res) {
            return *res;
        }
        throw TimeoutError("SOME/IP request 0x" + hex4(method_id) + " timed out");
    }

    bool subscribe(uint16_t service_id, uint16_t instance_id,
                   std::set<uint16_t> eventgroups, double ack_timeout = 5.0) {
        const auto deadline = now_plus(ack_timeout);
        const sdm::ServiceMonitor::SubState fast = fast_sub_state(
            service_id, instance_id, eventgroups);
        if (fast == sdm::ServiceMonitor::SubState::Ack) {
            return true;
        }
        if (fast == sdm::ServiceMonitor::SubState::Nack) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mx_);
            monitor_.find(service_id, instance_id, eventgroups);
        }
        {
            std::unique_lock<std::mutex> lk(mx_);
            const bool found = cv_.wait_until(lk, deadline, [&] {
                const auto *o = monitor_.offer({service_id, instance_id});
                return o != nullptr && o->available;
            });
            if (!found) {
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lk(mx_);
            monitor_.resubscribe(service_id, instance_id);
        }
        // The Ack/Nack for our renewed SUBSCRIBE may already be processed, or
        // will arrive shortly; subscription_state() reflects both. All reads
        // below happen while holding mx_, so no re-lock is needed.
        std::unique_lock<std::mutex> lk(mx_);
        const bool done = cv_.wait_until(lk, deadline, [&] {
            for (uint16_t g : eventgroups) {
                const auto st =
                    monitor_.subscription_state(service_id, instance_id, g);
                if (st == sdm::ServiceMonitor::SubState::Ack ||
                    st == sdm::ServiceMonitor::SubState::Nack) {
                    return true;
                }
            }
            return false;
        });
        if (!done) {
            return false;
        }
        for (uint16_t g : eventgroups) {
            const auto st =
                monitor_.subscription_state(service_id, instance_id, g);
            if (st == sdm::ServiceMonitor::SubState::Ack) {
                return true;
            }
            if (st == sdm::ServiceMonitor::SubState::Nack) {
                return false;
            }
        }
        return false;
    }

    // Re-emit SUBSCRIBE for an already-discovered service (recovery).
    void resubscribe(uint16_t service_id, uint16_t instance_id) {
        std::lock_guard<std::mutex> lk(mx_);
        monitor_.resubscribe(service_id, instance_id);
        cv_.notify_all();
    }

private:
    using steady_clock = std::chrono::steady_clock;

    struct Pending {
        uint16_t service_id = 0;
        steady_clock::time_point deadline;
        bool ready = false;
        bool stale = false;
        std::optional<std::pair<uint8_t, std::vector<uint8_t>>> result;
    };

    enum class EventKind { Available, Unavailable, SubOk, SubNack, Notify };
    struct Event {
        EventKind kind;
        uint16_t svc = 0;
        uint16_t inst = 0;
        uint16_t gid_or_event = 0;
        std::vector<uint8_t> payload;
    };

    static std::chrono::duration<double> secs(double s) {
        return std::chrono::duration<double>(s);
    }
    static steady_clock::time_point now_plus(double s) {
        return steady_clock::now() +
               std::chrono::duration_cast<steady_clock::duration>(secs(s));
    }
    static std::string hex4(uint16_t v) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04X", v);
        return std::string(buf);
    }
    static uint16_t random_client_id() {
        return uint16_t(1 + (std::random_device{}() % 0xFFFE));
    }

    uint16_t next_client_session() {
        session_ = (session_ + 1) & 0xFFFF;
        return session_;
    }

    sdm::ServiceMonitor::SubState fast_sub_state(
        uint16_t service_id, uint16_t instance_id,
        const std::set<uint16_t> &eventgroups) {
        std::lock_guard<std::mutex> lk(mx_);
        for (uint16_t g : eventgroups) {
            const auto st =
                monitor_.subscription_state(service_id, instance_id, g);
            if (st == sdm::ServiceMonitor::SubState::Ack ||
                st == sdm::ServiceMonitor::SubState::Nack) {
                return st;
            }
        }
        return sdm::ServiceMonitor::SubState::None;
    }

    void expire_pending_locked() {
        const auto now = steady_clock::now();
        for (auto &kv : pending_) {
            if (!kv.second.ready && kv.second.deadline < now) {
                kv.second.ready = true;
                kv.second.stale = true;
            }
        }
        if (!pending_.empty()) {
            cv_.notify_all();
        }
    }

    void dispatch(const Message &m) {
        const Header &h = m.header;
        if (h.message_type == MessageType::RESPONSE ||
            h.message_type == MessageType::ERROR) {
            auto it = pending_.find(h.session_id);
            if (it != pending_.end() && !it->second.ready &&
                it->second.service_id == h.service_id) {
                it->second.result =
                    std::make_pair(h.return_code, m.payload);
                it->second.ready = true;
                cv_.notify_all();
            }
            return;
        }
        if (h.message_type == MessageType::NOTIFICATION) {
            events_.push_back(
                Event{EventKind::Notify, 0, 0, h.method_id, m.payload});
            cv_.notify_all();
        }
    }

    void dispatch_buffered() {
        std::deque<Event> evs;
        {
            std::lock_guard<std::mutex> lk(mx_);
            evs.swap(events_);
        }
        for (const auto &e : evs) {
            if (e.kind == EventKind::Available) {
                std::vector<AvailFn> fns;
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    fns = avail_;
                }
                for (const auto &fn : fns) {
                    fn(e.svc, e.inst);
                }
            } else if (e.kind == EventKind::Unavailable) {
                std::vector<AvailFn> fns;
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    fns = unavail_;
                }
                for (const auto &fn : fns) {
                    fn(e.svc, e.inst);
                }
            } else if (e.kind == EventKind::SubOk) {
                GroupFn fn;
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    fn = sub_ok_;
                }
                if (fn) {
                    fn(e.svc, e.inst, e.gid_or_event);
                }
            } else if (e.kind == EventKind::SubNack) {
                GroupFn fn;
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    fn = sub_nack_;
                }
                if (fn) {
                    fn(e.svc, e.inst, e.gid_or_event);
                }
            } else if (e.kind == EventKind::Notify) {
                EventFn fn;
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    const auto it = event_cb_.find(e.gid_or_event);
                    if (it != event_cb_.end()) {
                        fn = it->second;
                    }
                }
                if (fn) {
                    fn(e.gid_or_event, e.payload);
                }
            }
        }
    }

    void client_loop() {
        ReassemblePass recon;
        std::vector<uint8_t> raw;
        sockaddr_in src;
        while (!stop_.load()) {
            bool anything = false;
            if (sd_ep_ && sd_ep_->recv(raw, src, 20)) {
                anything = true;
                try {
                    const Message m = Message::from_bytes(raw.data(), raw.size());
                    std::lock_guard<std::mutex> lk(mx_);
                    monitor_.handle_datagram(m, host_from(src));
                } catch (const WireError &) {
                }
            }
            if (endpoint_ && endpoint_->recv(raw, src, 20)) {
                anything = true;
                try {
                    const Message m = Message::from_bytes(raw.data(), raw.size());
                    if (auto full = recon.feed(m)) {
                        std::lock_guard<std::mutex> lk(mx_);
                        dispatch(*full);
                    }
                } catch (const WireError &) {
                }
            }
            std::vector<Message> sd_msgs;
            {
                std::lock_guard<std::mutex> lk(mx_);
                monitor_.tick(sdm::ServicePublisher::default_now());
                sd_msgs = monitor_.process(sdm::ServicePublisher::default_now());
                expire_pending_locked();
            }
            for (const auto &m : sd_msgs) {
                std::vector<uint8_t> raw2;
                m.to_bytes(raw2);
                try {
                    sd_ep_->send(raw2.data(), raw2.size(),
                                 sd_destination(sd_port_));
                } catch (const TransportError &) {
                }
            }
            recon.tick();
            dispatch_buffered();
            if (!anything) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    uint16_t client_id_;
    uint16_t sd_port_;
    std::string interface_ip_;

    sdm::ServiceMonitor monitor_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    uint16_t session_ = 0;
    std::map<uint16_t, Pending> pending_;

    std::map<uint16_t, EventFn> event_cb_;
    std::vector<AvailFn> avail_;
    std::vector<AvailFn> unavail_;
    GroupFn sub_ok_;
    GroupFn sub_nack_;
    std::deque<Event> events_;

    std::unique_ptr<UdpEndpoint> endpoint_;
    std::unique_ptr<UdpEndpoint> sd_ep_;
    std::thread thread_;
};

} // namespace app
} // namespace someip

#endif // SOMEIP_APP_HPP
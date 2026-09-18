// C++ v2 Service Discovery tests (mirrors tests/test_py_sdm.py).
#include "someip/sdm.hpp"

#include <cstdio>
#include <cstring>
#include <string>
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
        catch (const someip::sdm::SdError &) { threw = true; }            \
        CHECK(threw);                                                     \
    } while (0)

// Deterministic wall clock (seconds).
struct FakeClock {
    double t = 1000.0;
    double operator()() { return t; }
    void advance(double d) { t += d; }
};

static const unsigned char GOLDEN_ENTRY[] = {
    0x01, 0x00, 0x00, 0x01,   // OFFER, idx 0/0, 1 option
    0x12, 0x34, 0x56, 0x78,   // service 0x1234, instance 0x5678
    0x01, 0x00, 0x00, 0x3C,   // major 1, ttl 60
    0x00, 0x00, 0x00, 0x01,   // minor 1
};
static const unsigned char GOLDEN_OPTION[] = {
    0x00, 0x09, 0x04, 0x00,   // len 9, IPV4_ENDPOINT, reserved
    0x0A, 0x00, 0x00, 0x05,   // 10.0.0.5
    0x11, 0x77, 0x24,         // protocol 0x11, port 30500
};

static someip::sdm::OfferedService make_service() {
    someip::sdm::OfferedService o;
    o.service_id = 0x1234;
    o.instance_id = 0x5678;
    o.major = 0x01;
    o.minor = 0x00000001;
    o.address = "10.0.0.5";
    o.port = 30500;
    o.eventgroups.insert(0x0001);
    return o;
}

static void test_codec_golden() {
    auto entry = someip::sdm::encode_entry(
        someip::sdm::SdEntryType::OFFER, 0x1234, 0x5678, 0x01, 60, 0x1, 0xFFFF,
        0, 0, 1);
    CHECK(entry.size() == 16);
    CHECK(std::memcmp(entry.data(), GOLDEN_ENTRY, 16) == 0);

    auto opt = someip::sdm::encode_option_ipv4("10.0.0.5", 30500);
    CHECK(opt.size() == 11);
    CHECK(std::memcmp(opt.data(), GOLDEN_OPTION, 11) == 0);

    someip::sdm::SdEntry e = someip::sdm::parse_entry(GOLDEN_ENTRY, 16);
    CHECK(e.type == someip::sdm::SdEntryType::OFFER);
    CHECK(e.service_id == 0x1234);
    CHECK(e.instance_id == 0x5678);
    CHECK(e.major_version == 0x01);
    CHECK(e.ttl == 60);
    CHECK(e.minor_version == 1);
    CHECK(e.opts1 == 1);
}

static void test_parse_message_roundtrip() {
    std::vector<std::vector<uint8_t>> entries = {
        someip::sdm::encode_entry(someip::sdm::SdEntryType::OFFER, 0x1234, 0x5678,
                                  0x01, 60, 0x00000001, 0xFFFF, 0, 0, 1)};
    std::vector<std::vector<uint8_t>> options = {
        someip::sdm::encode_option_ipv4("10.0.0.5", 30500)};
    someip::Message m = someip::sdm::build_sd_message(entries, options, 0x0001, 7);
    auto parsed = someip::sdm::parse_sd_message(m);
    CHECK(parsed.size() == 1);
    CHECK(parsed[0].service_id == 0x1234);
    CHECK(parsed[0].ttl == 60);
    CHECK(parsed[0].options.size() == 1);
    auto ep = someip::sdm::ipv4_endpoint(parsed[0].options[0]);
    CHECK(ep.first == "10.0.0.5" && ep.second == 30500);
}

static void test_publisher_backoff_and_find() {
    FakeClock clock;
    someip::sdm::ServicePublisher pub({make_service()}, clock);
    clock.advance(1.0);
    auto msgs = pub.process(clock());
    CHECK(msgs.size() == 1);
    auto entries = someip::sdm::parse_sd_message(msgs[0]);
    CHECK(entries[0].type == someip::sdm::SdEntryType::OFFER);

    clock.advance(0.1);
    CHECK(pub.process(clock()).empty());

    std::vector<std::vector<uint8_t>> find_entries = {
        someip::sdm::encode_entry(someip::sdm::SdEntryType::FIND, 0x1234, 0x5678,
                                  0x01, 3, 0)};
    someip::Message find = someip::sdm::build_sd_message(find_entries, {}, 0x0001, 1);
    pub.handle_datagram(find, "10.0.0.2");
    auto resp = pub.drain_pending();
    CHECK(resp.size() == 1);
    auto r_entries = someip::sdm::parse_sd_message(resp[0]);
    CHECK(r_entries[0].type == someip::sdm::SdEntryType::OFFER);
}

static void test_publisher_subscribe_nack_and_stop() {
    FakeClock clock;
    someip::sdm::ServicePublisher pub({make_service()}, clock);
    std::vector<std::vector<uint8_t>> sub_entries = {
        someip::sdm::encode_entry(someip::sdm::SdEntryType::SUBSCRIBE, 0x1234,
                                  0x5678, 0x01, 3, 0, 0x0001, 0, 0, 1)};
    std::vector<std::vector<uint8_t>> options = {
        someip::sdm::encode_option_ipv4("10.0.0.2", 30501)};
    someip::Message sub = someip::sdm::build_sd_message(sub_entries, options, 0x0001, 1);
    pub.handle_datagram(sub, "10.0.0.2");
    auto resp = pub.drain_pending();
    CHECK(resp.size() == 1);
    auto ack = someip::sdm::parse_sd_message(resp[0]);
    CHECK(ack[0].type == someip::sdm::SdEntryType::SUBSCRIBE_ACK);
    CHECK(ack[0].eventgroup_id == 0x0001);
    CHECK(pub.subscribers(0x1234, 0x5678).count("10.0.0.2") == 1);

    // unknown group -> Nack
    std::vector<std::vector<uint8_t>> bad_entries = {
        someip::sdm::encode_entry(someip::sdm::SdEntryType::SUBSCRIBE, 0x1234,
                                  0x5678, 0x01, 3, 0, 0x0099, 0, 0, 1)};
    someip::Message bad = someip::sdm::build_sd_message(bad_entries, options, 0x0001, 2);
    pub.handle_datagram(bad, "10.0.0.2");
    auto nack = someip::sdm::parse_sd_message(pub.drain_pending()[0]);
    CHECK(nack[0].type == someip::sdm::SdEntryType::SUBSCRIBE_NACK);

    // stop -> offer with ttl 0
    auto stop_raw = pub.stop()[0];
    auto stop = someip::sdm::parse_sd_message(stop_raw)[0];
    CHECK(stop.type == someip::sdm::SdEntryType::OFFER);
    CHECK(stop.ttl == 0);
}

static void test_publisher_sawtooth() {
    FakeClock clock;
    someip::sdm::ServicePublisher pub({make_service()}, clock);
    auto find5 = someip::sdm::build_sd_message(
        {someip::sdm::encode_entry(someip::sdm::SdEntryType::FIND, 0x1234, 0x5678,
                                   0x01, 3, 0)},
        {}, 0x0001, 5);
    pub.handle_datagram(find5, "10.0.0.2");
    CHECK(pub.drain_pending().size() == 1);
    auto find3 = someip::sdm::build_sd_message(
        {someip::sdm::encode_entry(someip::sdm::SdEntryType::FIND, 0x1234, 0x5678,
                                   0x01, 3, 0)},
        {}, 0x0001, 3);
    pub.handle_datagram(find3, "10.0.0.2");
    CHECK(pub.drain_pending().empty());
}

static void test_monitor_offer_and_subscribe() {
    FakeClock clock;
    someip::sdm::ServiceMonitor mon(0x0001, clock);
    mon.set_client_endpoint("10.0.0.2", 30501);
    mon.find(0x1234, 0x5678, {0x0001});
    bool available = false, sub_ok = false;
    mon.on_available([&](uint16_t, uint16_t) { available = true; });
    mon.on_subscribe_ok([&](uint16_t s, uint16_t i, uint16_t g) {
        sub_ok = (s == 0x1234 && i == 0x5678 && g == 0x0001);
    });

    clock.advance(1.0);
    auto finds = mon.process(clock());
    CHECK(finds.size() == 1);
    CHECK(someip::sdm::parse_sd_message(finds[0])[0].type ==
          someip::sdm::SdEntryType::FIND);

    // service offers itself (construct before advancing so its first offer
    // is due after the initial delay)
    someip::sdm::ServicePublisher pub({make_service()}, clock);
    clock.advance(1.0);
    auto offer_msgs = pub.process(clock());
    CHECK(offer_msgs.size() == 1);
    someip::Message offer_msg = offer_msgs[0];
    mon.handle_datagram(offer_msg, "10.0.0.5");
    CHECK(available);
    const someip::sdm::ServiceMonitor::Offer *o =
        mon.offer({0x1234, 0x5678});
    CHECK(o != nullptr && o->address == "10.0.0.5");

    // the first SUBSCRIBE is emitted immediately upon the offer
    auto subs = mon.drain_pending();
    CHECK(!subs.empty());
    if (subs.empty()) {
        return;
    }
    auto sub_entries = someip::sdm::parse_sd_message(subs[0]);
    CHECK(sub_entries[0].type == someip::sdm::SdEntryType::SUBSCRIBE);

    // publisher acks -> monitor fires callback
    pub.handle_datagram(subs[0], "10.0.0.2");
    auto acks = pub.drain_pending();
    CHECK(!acks.empty());
    mon.handle_datagram(acks[0], "10.0.0.5");
    CHECK(sub_ok);
}

static void test_monitor_ttl_expiry_and_stop() {
    FakeClock clock;
    someip::sdm::ServiceMonitor mon(0x0001, clock);
    mon.find(0x1234, 0x5678);
    int unavailable = 0;
    mon.on_unavailable([&](uint16_t, uint16_t) { ++unavailable; });

    someip::sdm::OfferedService o = make_service();
    std::vector<std::vector<uint8_t>> entries = {
        someip::sdm::encode_entry(someip::sdm::SdEntryType::OFFER, 0x1234, 0x5678,
                                  o.major, 2, o.minor, 0xFFFF, 0, 0, 1)};
    std::vector<std::vector<uint8_t>> options = {
        someip::sdm::encode_option_ipv4(o.address.c_str(), o.port)};
    someip::Message offer = someip::sdm::build_sd_message(entries, options, 0, 1);
    mon.handle_datagram(offer, "10.0.0.5");
    CHECK(unavailable == 0);
    clock.advance(2.5);
    mon.tick(clock());
    CHECK(unavailable == 1);

    // new offer, then StopOffer(ttl=0) -> immediate unavailable
    mon.handle_datagram(offer, "10.0.0.5");
    std::vector<std::vector<uint8_t>> stop_entries = {
        someip::sdm::encode_entry(someip::sdm::SdEntryType::OFFER, 0x1234,
                                  0x5678, o.major, 0, o.minor, 0xFFFF)};
    someip::Message stop = someip::sdm::build_sd_message(stop_entries, {}, 0, 1);
    mon.handle_datagram(stop, "10.0.0.5");
    CHECK(unavailable == 2);
}

int main() {
    test_codec_golden();
    test_parse_message_roundtrip();
    test_publisher_backoff_and_find();
    test_publisher_subscribe_nack_and_stop();
    test_publisher_sawtooth();
    test_monitor_offer_and_subscribe();
    test_monitor_ttl_expiry_and_stop();
    if (failures == 0) {
        std::printf("C++ v2 sdm: ALL PASSED\n");
        return 0;
    }
    std::printf("C++ v2 sdm: %d failure(s)\n", failures);
    return 1;
}
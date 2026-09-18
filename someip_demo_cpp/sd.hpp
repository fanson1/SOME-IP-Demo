#ifndef SOMEIP_SD_HPP
#define SOMEIP_SD_HPP

#include <cstdint>
#include <vector>
#include <string>
#include "someip.hpp"

namespace sd {

const uint16_t SD_SERVICE_ID = 0xFFFF;
const uint16_t SD_METHOD_ID = 0x8100;
const char    *SD_MULTICAST_ADDRESS = "224.244.224.245";
const uint16_t SD_PORT = 30490;
const uint32_t SD_TTL = 3;

struct EntryType {
    static const uint8_t FIND_SERVICE             = 0x00;
    static const uint8_t OFFER_SERVICE            = 0x01;
    static const uint8_t SUBSCRIBE_EVENTGROUP     = 0x06;
    static const uint8_t SUBSCRIBE_EVENTGROUP_ACK = 0x07;
};

struct OptionType {
    static const uint8_t IPV4_ENDPOINT = 0x04;
};

const uint8_t L4_UDP = 0x11;

inline void build_ipv4_endpoint_option(const std::string &address, uint16_t port,
                                       std::vector<uint8_t> &out) {
    out.push_back(0x00); out.push_back(0x09);   // length = 9
    out.push_back(OptionType::IPV4_ENDPOINT);
    out.push_back(0x00);                        // reserved
    unsigned a = 0, b = 0, c = 0, d = 0;
    std::sscanf(address.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
    out.push_back(uint8_t(a)); out.push_back(uint8_t(b));
    out.push_back(uint8_t(c)); out.push_back(uint8_t(d));
    out.push_back(L4_UDP);                      // L4 protocol
    someip::push_u16(out, port);
}

inline void build_entry(uint8_t type, uint16_t service_id, uint16_t instance_id,
                        uint8_t major, uint32_t ttl, uint32_t minor,
                        uint16_t eventgroup_id, uint8_t index_first,
                        uint8_t n_options, std::vector<uint8_t> &out) {
    uint8_t counts = n_options & 0x0F;
    out.push_back(type);
    out.push_back(index_first);
    out.push_back(0x00);                        // index second option run
    out.push_back(counts);
    someip::push_u16(out, service_id);
    someip::push_u16(out, instance_id);
    out.push_back(major);
    out.push_back(uint8_t(ttl >> 16));
    out.push_back(uint8_t(ttl >> 8));
    out.push_back(uint8_t(ttl));
    if (type == EntryType::SUBSCRIBE_EVENTGROUP ||
        type == EntryType::SUBSCRIBE_EVENTGROUP_ACK) {
        someip::push_u16(out, eventgroup_id);
        someip::push_u16(out, 0x0000);
    } else {
        someip::push_u32(out, minor);
    }
}

inline void build_sd_message(const std::vector<std::vector<uint8_t>> &entries,
                             const std::vector<std::vector<uint8_t>> &options,
                             uint16_t session_id, std::vector<uint8_t> &out) {
    std::vector<uint8_t> entries_raw;
    for (const auto &e : entries) entries_raw.insert(entries_raw.end(), e.begin(), e.end());
    std::vector<uint8_t> options_raw;
    for (const auto &o : options) options_raw.insert(options_raw.end(), o.begin(), o.end());

    std::vector<uint8_t> payload;
    someip::push_u32(payload, uint32_t(entries_raw.size()));
    payload.insert(payload.end(), entries_raw.begin(), entries_raw.end());
    while (payload.size() % 4 != 0) payload.push_back(0);
    someip::push_u32(payload, uint32_t(options_raw.size()));
    payload.insert(payload.end(), options_raw.begin(), options_raw.end());
    while (payload.size() % 4 != 0) payload.push_back(0);

    someip::Message msg;
    msg.service_id = SD_SERVICE_ID;
    msg.method_id = SD_METHOD_ID;
    msg.client_id = 0x0000;
    msg.session_id = session_id;
    msg.message_type = someip::MessageType::NOTIFICATION;
    msg.return_code = someip::ReturnCode::E_OK;
    msg.interface_version = 0x01;
    msg.payload = std::move(payload);
    msg.to_buf(out);
}

} // namespace sd

#endif // SOMEIP_SD_HPP
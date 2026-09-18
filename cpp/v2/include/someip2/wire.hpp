#ifndef SOMEIP2_WIRE_HPP
#define SOMEIP2_WIRE_HPP

#include "types.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace someip2 {

// Header is the 16-byte fixed part of a SOME/IP message.
struct Header {
    uint16_t service_id = 0;
    uint16_t method_id = 0;
    uint16_t client_id = 0;
    uint16_t session_id = 0;
    uint8_t  protocol_version = PROTOCOL_VERSION;
    uint8_t  interface_version = 0x01;
    uint8_t  message_type = MessageType::REQUEST;
    uint8_t  return_code = 0;
    uint32_t length_field = 0; // raw header field (8 + payload bytes)

    uint32_t message_id() const { return (uint32_t(service_id) << 16) | method_id; }
    uint32_t request_id() const { return (uint32_t(client_id) << 16) | session_id; }
    bool is_notification() const { return message_type == MessageType::NOTIFICATION; }
    bool is_tp() const { return (message_type & 0x20) != 0; }
};

// Message = validated Header + payload.
struct Message {
    Header header;
    std::vector<uint8_t> payload;

    static Header decode_header(const uint8_t *p, size_t size) {
        if (size < 16) {
            throw MalformedMessage("packet shorter than 16-byte header");
        }
        Header h;
        h.service_id = uint16_t(p[0] << 8) | p[1];
        h.method_id = uint16_t(p[2] << 8) | p[3];
        h.length_field = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16)
                       | (uint32_t(p[6]) << 8) | uint32_t(p[7]);
        if (h.length_field < 8) {
            throw MalformedMessage("invalid length field " + std::to_string(h.length_field));
        }
        h.client_id = uint16_t(p[8] << 8) | p[9];
        h.session_id = uint16_t(p[10] << 8) | p[11];
        h.protocol_version = p[12];
        h.interface_version = p[13];
        h.message_type = p[14];
        h.return_code = p[15];
        return h;
    }

    // Serialize exactly like the wire spec (and v1 stack).
    void to_bytes(std::vector<uint8_t> &out) const {
        out.resize(16 + payload.size());
        uint8_t *p = out.data();
        p[0] = uint8_t(header.service_id >> 8);  p[1] = uint8_t(header.service_id);
        p[2] = uint8_t(header.method_id >> 8);   p[3] = uint8_t(header.method_id);
        const uint32_t len = 8 + uint32_t(payload.size());
        p[4] = uint8_t(len >> 24); p[5] = uint8_t(len >> 16);
        p[6] = uint8_t(len >> 8);  p[7] = uint8_t(len);
        p[8] = uint8_t(header.client_id >> 8);   p[9] = uint8_t(header.client_id);
        p[10] = uint8_t(header.session_id >> 8); p[11] = uint8_t(header.session_id);
        p[12] = header.protocol_version;
        p[13] = header.interface_version;
        p[14] = header.message_type;
        p[15] = header.return_code;
        std::memcpy(p + 16, payload.data(), payload.size());
    }

    // Parse a complete datagram. If not all payload bytes are present yet,
    // MalformedMessage with a descriptive message is thrown (stream assembly
    // is handled by the transport layer).
    static Message from_bytes(const uint8_t *data, size_t size) {
        Header h = decode_header(data, size);
        const size_t expected = size_t(h.length_field) - 8;
        if (16 + expected > size) {
            throw MalformedMessage(
                "partial message: need " + std::to_string(16 + expected) +
                " bytes, have " + std::to_string(size));
        }
        Message m;
        m.header = h;
        m.payload.assign(data + 16, data + 16 + expected);
        return m;
    }
};

} // namespace someip2

#endif // SOMEIP2_WIRE_HPP
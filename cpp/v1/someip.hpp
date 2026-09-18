#ifndef SOMEIP_HPP
#define SOMEIP_HPP

#include <cstdint>
#include <vector>
#include <string>

namespace someip {

const uint8_t PROTOCOL_VERSION = 0x01;

struct MessageType {
    static const uint8_t REQUEST           = 0x00;
    static const uint8_t REQUEST_NO_RETURN = 0x01;
    static const uint8_t NOTIFICATION      = 0x02;
    static const uint8_t RESPONSE          = 0x80;
    static const uint8_t ERROR             = 0x81;
};

struct ReturnCode {
    static const uint8_t E_OK                     = 0x00;
    static const uint8_t E_NOT_OK                 = 0x01;
    static const uint8_t E_UNKNOWN_SERVICE        = 0x02;
    static const uint8_t E_UNKNOWN_METHOD         = 0x03;
    static const uint8_t E_NOT_READY              = 0x04;
    static const uint8_t E_MALFORMED_MESSAGE      = 0x09;
};

struct Message {
    uint16_t service_id = 0;
    uint16_t method_id = 0;
    uint16_t client_id = 0;
    uint16_t session_id = 0;
    uint8_t  protocol_version = PROTOCOL_VERSION;
    uint8_t  interface_version = 0x01;
    uint8_t  message_type = 0;
    uint8_t  return_code = 0;
    std::vector<uint8_t> payload;

    uint32_t message_id() const { return (uint32_t(service_id) << 16) | method_id; }
    uint32_t request_id() const { return (uint32_t(client_id) << 16) | session_id; }
    uint32_t length() const { return 8u + uint32_t(payload.size()); }

    void to_buf(std::vector<uint8_t> &out) const {
        out.resize(16 + payload.size());
        uint8_t *p = out.data();
        p[0] = uint8_t(service_id >> 8);  p[1] = uint8_t(service_id);
        p[2] = uint8_t(method_id >> 8);   p[3] = uint8_t(method_id);
        uint32_t len = length();
        p[4] = uint8_t(len >> 24);  p[5] = uint8_t(len >> 16);
        p[6] = uint8_t(len >> 8);   p[7] = uint8_t(len);
        p[8] = uint8_t(client_id >> 8);   p[9] = uint8_t(client_id);
        p[10] = uint8_t(session_id >> 8); p[11] = uint8_t(session_id);
        p[12] = protocol_version;
        p[13] = interface_version;
        p[14] = message_type;
        p[15] = return_code;
        for (size_t i = 0; i < payload.size(); ++i) p[16 + i] = payload[i];
    }

    static bool from_buf(const uint8_t *data, size_t size, Message &out) {
        if (size < 16) return false;
        out.service_id = uint16_t((data[0] << 8) | data[1]);
        out.method_id = uint16_t((data[2] << 8) | data[3]);
        uint32_t len = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16)
                     | (uint32_t(data[6]) << 8) | uint32_t(data[7]);
        out.client_id = uint16_t((data[8] << 8) | data[9]);
        out.session_id = uint16_t((data[10] << 8) | data[11]);
        out.protocol_version = data[12];
        out.interface_version = data[13];
        out.message_type = data[14];
        out.return_code = data[15];
        size_t paylen = len >= 8 ? size_t(len - 8) : 0;
        if (16 + paylen > size) paylen = size - 16;
        out.payload.assign(data + 16, data + 16 + paylen);
        return true;
    }
};

inline uint16_t load_u16(const uint8_t *p) {
    return uint16_t((p[0] << 8) | p[1]);
}

inline uint32_t load_u32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline void push_u16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

inline void push_u32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}

} // namespace someip

#endif // SOMEIP_HPP
#ifndef SOMEIP_TYPES_HPP
#define SOMEIP_TYPES_HPP

#include <stdexcept>
#include <cstdint>

namespace someip {

constexpr uint8_t PROTOCOL_VERSION = 0x01;
constexpr size_t MAX_PAYLOAD_NO_TP = 1392;

struct MessageType {
    static constexpr uint8_t REQUEST           = 0x00;
    static constexpr uint8_t REQUEST_NO_RETURN = 0x01;
    static constexpr uint8_t NOTIFICATION      = 0x02;
    static constexpr uint8_t RESPONSE          = 0x80;
    static constexpr uint8_t ERROR             = 0x81;
};

struct TpFlag {
    static constexpr uint8_t TP_REQUEST           = 0x20;
    static constexpr uint8_t TP_RESPONSE          = 0x21;
    static constexpr uint8_t TP_ERROR             = 0x22;
    static constexpr uint8_t TP_NOTIFICATION      = 0x23;
    static constexpr uint8_t TP_REQUEST_NO_RETURN = 0x24;
};

struct ReturnCode {
    static constexpr uint8_t E_OK                     = 0x00;
    static constexpr uint8_t E_NOT_OK                 = 0x01;
    static constexpr uint8_t E_UNKNOWN_SERVICE        = 0x02;
    static constexpr uint8_t E_UNKNOWN_METHOD         = 0x03;
    static constexpr uint8_t E_NOT_READY              = 0x04;
    static constexpr uint8_t E_MALFORMED_MESSAGE      = 0x09;
};

class WireError : public std::runtime_error {
public:
    explicit WireError(const std::string &what) : std::runtime_error(what) {}
};

class MalformedMessage : public WireError {
public:
    explicit MalformedMessage(const std::string &what) : WireError(what) {}
};

} // namespace someip

#endif // SOMEIP_TYPES_HPP
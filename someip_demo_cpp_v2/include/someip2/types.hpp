#ifndef SOMEIP2_TYPES_HPP
#define SOMEIP2_TYPES_HPP

#include <stdexcept>
#include <cstdint>

namespace someip2 {

constexpr uint8_t PROTOCOL_VERSION = 0x01;
constexpr size_t MAX_PAYLOAD_NO_TP = 1392;

struct MessageType {
    static constexpr uint8_t REQUEST           = 0x00;
    static constexpr uint8_t REQUEST_NO_RETURN = 0x01;
    static constexpr uint8_t NOTIFICATION      = 0x02;
    static constexpr uint8_t RESPONSE          = 0x80;
    static constexpr uint8_t ERROR             = 0x81;
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

} // namespace someip2

#endif // SOMEIP2_TYPES_HPP
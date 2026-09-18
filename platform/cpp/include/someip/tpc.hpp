#ifndef SOMEIP_TPC_HPP
#define SOMEIP_TPC_HPP

// SOME/IP-TP: segmentation and reassembly (UDP MTU-safe chunking).
// Mirrors platform/python/someip/tpc.py (C++17, no third-party deps).
//
// Wire layout of a TP segment (16-byte SOME/IP header + 4-byte TP header +
// segment payload):
//     SOME/IP header length field = 8 + 4 + segment_payload_size
//     message_type is the TP variant (TP_REQUEST / TP_RESPONSE / ...).
//     TP header bits (big-endian u32):
//         [31..16] reserved (must be 0)
//         [15]     MoreSegmentsFlag
//         [14..0]  offset of this segment, in 16-byte units
//
// Sessions are keyed by (service, method, client, session) and evicted on a
// stale timeout (tick()), bounding memory under replay conditions.

#include "types.hpp"
#include "wire.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace someip {

static constexpr size_t TP_HEADER_SIZE = 4;
static constexpr size_t TP_MAX_SEGMENT = 1392;      // multiple of 16
static constexpr size_t TP_MAX_SESSIONS = 4096;
static constexpr double TP_DEFAULT_TIMEOUT_S = 5.0;

struct TpError : std::runtime_error {
    explicit TpError(const std::string &what) : std::runtime_error(what) {}
};
struct TpOutOfOrder : TpError {
    explicit TpOutOfOrder(const std::string &what) : TpError(what) {}
};

class TpHeader {
public:
    bool more = false;
    uint16_t offset = 0;

    void to_bytes(uint8_t *out) const {
        const uint32_t field = (more ? 0x8000u : 0u) | offset;
        out[0] = uint8_t(field >> 24);
        out[1] = uint8_t(field >> 16);
        out[2] = uint8_t(field >> 8);
        out[3] = uint8_t(field);
    }

    static TpHeader from_bytes(const uint8_t *p, size_t size) {
        if (size < TP_HEADER_SIZE) {
            throw TpError("short TP header");
        }
        const uint32_t field = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
                             | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        if ((field & 0xFFFF0000u) != 0) {
            throw TpError("reserved bits set in TP header");
        }
        TpHeader h;
        h.more = (field & 0x8000u) != 0;
        h.offset = uint16_t(field & 0x7FFFu);
        return h;
    }
};

// Segment a complete message into frames. Small messages are returned as-is.
inline std::vector<Message> segment(const Message &msg, size_t max_segment = TP_MAX_SEGMENT) {
    if (msg.header.is_tp()) {
        throw TpError("cannot segment an already-TP message");
    }
    if (msg.payload.size() <= max_segment) {
        return {msg};
    }
    uint8_t tp_msg_type = 0;
    switch (msg.header.message_type) {
        case MessageType::REQUEST:             tp_msg_type = TpFlag::TP_REQUEST; break;
        case MessageType::RESPONSE:            tp_msg_type = TpFlag::TP_RESPONSE; break;
        case MessageType::ERROR:               tp_msg_type = TpFlag::TP_ERROR; break;
        case MessageType::NOTIFICATION:        tp_msg_type = TpFlag::TP_NOTIFICATION; break;
        case MessageType::REQUEST_NO_RETURN:   tp_msg_type = TpFlag::TP_REQUEST_NO_RETURN; break;
        default:
            throw TpError("cannot segment message type 0x" +
                          std::to_string(msg.header.message_type));
    }
    std::vector<Message> out;
    for (size_t off = 0; off < msg.payload.size(); off += max_segment) {
        const size_t n = std::min(max_segment, msg.payload.size() - off);
        Message tp;
        tp.header = msg.header;
        tp.header.message_type = tp_msg_type;
        tp.payload.resize(TP_HEADER_SIZE + n);
        TpHeader hdr;
        hdr.more = (off + n < msg.payload.size());
        hdr.offset = uint16_t(off);
        hdr.to_bytes(tp.payload.data());
        std::memcpy(tp.payload.data() + TP_HEADER_SIZE, msg.payload.data() + off, n);
        out.push_back(std::move(tp));
    }
    return out;
}

class Reassembler {
public:
    explicit Reassembler(double timeout_s = TP_DEFAULT_TIMEOUT_S,
                         size_t max_sessions = TP_MAX_SESSIONS)
        : timeout_s_(timeout_s), max_sessions_(max_sessions) {}

    size_t size() const { return sessions_.size(); }

    void tick(double now_epoch_ms = -1.0) {
        const double now = now_epoch_ms < 0 ? now_ms() : now_epoch_ms;
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (now - it->second.last_update > timeout_s_ * 1000.0) {
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Feed one wire frame. Non-TP frames pass through unchanged. TP frames
    // accumulate; the full reconstructed Message is returned on the final
    // segment, otherwise std::nullopt. A gap throws TpOutOfOrder.
    std::optional<Message> add(const Message &frame, bool passthrough = true) {
        if (!frame.header.is_tp()) {
            if (!passthrough) {
                return std::nullopt;
            }
            return frame;
        }
        const uint64_t key = session_key(frame.header);
        const TpHeader tp = TpHeader::from_bytes(frame.payload.data(), frame.payload.size());
        std::vector<uint8_t> data(frame.payload.begin() + TP_HEADER_SIZE,
                                  frame.payload.end());
        if (tp.offset == 0) {
            Session s{};
            s.last_update = now_ms();
            sessions_[key] = std::move(s);  // (re)start session
        }
        auto it = sessions_.find(key);
        if (it == sessions_.end()) {
            throw TpOutOfOrder("segment before session start");
        }
        Session &s = it->second;
        if (tp.offset != s.next_offset) {
            sessions_.erase(key);
            throw TpOutOfOrder("expected offset " + std::to_string(s.next_offset) +
                               ", got " + std::to_string(tp.offset));
        }
        s.chunks.push_back(std::move(data));
        s.next_offset += s.chunks.back().size();
        s.last_update = now_ms();
        if (sessions_.size() > max_sessions_) {
            evict_oldest();
        }
        if (!tp.more) {
            Message full;
            full.header = frame.header;
            full.header.message_type = restore_message_type(frame.header.message_type);
            full.payload.clear();
            for (const auto &c : s.chunks) {
                full.payload.insert(full.payload.end(), c.begin(), c.end());
            }
            sessions_.erase(key);
            return full;
        }
        return std::nullopt;
    }

private:
    struct Session {
        std::vector<std::vector<uint8_t>> chunks;
        size_t next_offset = 0;
        double last_update = 0.0;
    };

    static uint8_t restore_message_type(uint8_t type) {
        switch (type) {
            case TpFlag::TP_REQUEST:           return MessageType::REQUEST;
            case TpFlag::TP_RESPONSE:          return MessageType::RESPONSE;
            case TpFlag::TP_ERROR:             return MessageType::ERROR;
            case TpFlag::TP_NOTIFICATION:      return MessageType::NOTIFICATION;
            case TpFlag::TP_REQUEST_NO_RETURN: return MessageType::REQUEST_NO_RETURN;
            default:
                throw TpError("unknown TP message type 0x" + std::to_string(type));
        }
    }

    static uint64_t session_key(const Header &h) {
        return (uint64_t(h.service_id) << 48) | (uint64_t(h.method_id) << 32) |
               (uint64_t(h.client_id) << 16) | uint64_t(h.session_id);
    }

    void evict_oldest() {
        auto oldest = sessions_.begin();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (it->second.last_update < oldest->second.last_update) {
                oldest = it;
            }
        }
        sessions_.erase(oldest);
    }

    static double now_ms() {
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    double timeout_s_;
    size_t max_sessions_;
    std::map<uint64_t, Session> sessions_;
};

} // namespace someip

#endif // SOMEIP_TPC_HPP
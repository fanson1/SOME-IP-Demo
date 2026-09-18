#ifndef SOMEIP2_SER_HPP
#define SOMEIP2_SER_HPP

#include "types.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace someip2 {

// Writer emits big-endian AUTOSAR wire-format primitives.
class Writer {
public:
    std::vector<uint8_t> data() const { return buf_; }

    Writer &u8(uint8_t v)  { buf_.push_back(v); return *this; }
    Writer &u16(uint16_t v) {
        buf_.push_back(uint8_t(v >> 8));
        buf_.push_back(uint8_t(v));
        return *this;
    }
    Writer &u32(uint32_t v) {
        buf_.push_back(uint8_t(v >> 24));
        buf_.push_back(uint8_t(v >> 16));
        buf_.push_back(uint8_t(v >> 8));
        buf_.push_back(uint8_t(v));
        return *this;
    }
    Writer &u64(uint64_t v) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            buf_.push_back(uint8_t(v >> shift));
        }
        return *this;
    }
    Writer &i8(int8_t v)  { return u8(uint8_t(v)); }
    Writer &i16(int16_t v) { return u16(uint16_t(v)); }
    Writer &i32(int32_t v) { return u32(uint32_t(v)); }
    Writer &i64(int64_t v) { return u64(uint64_t(v)); }
    Writer &f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        return u32(bits);
    }
    Writer &f64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        return u64(bits);
    }
    Writer &boolean(bool v) { return u8(v ? 1 : 0); }

    // String: u16(length incl. NUL) + UTF-8 bytes + NUL.
    Writer &string(const std::string &s) {
        u16(uint16_t(s.size() + 1));
        buf_.insert(buf_.end(), s.begin(), s.end());
        buf_.push_back(0);
        return *this;
    }
    Writer &bytes(const uint8_t *p, uint16_t n) {
        u16(n);
        buf_.insert(buf_.end(), p, p + n);
        return *this;
    }
    // Struct: known layout, sequential writer_fn.
    Writer &struct_(const std::function<void(Writer &)> &fn) {
        fn(*this);
        return *this;
    }

private:
    std::vector<uint8_t> buf_;
};

// Reader parses big-endian wire format with bounds checks (throws on any
// out-of-bounds read rather than returning garbage).
class Reader {
public:
    // Owns the buffer by value: avoids dangling references to Writer::data()
    // temporaries and keeps reads stable.
    explicit Reader(const std::vector<uint8_t> &data) : data_(data) {}

    size_t remaining() const { return data_.size() - pos_; }

    uint8_t u8() {
        take(1);
        return data_[pos_ - 1];
    }
    uint16_t u16() {
        take(2);
        return uint16_t(data_[pos_ - 2] << 8) | data_[pos_ - 1];
    }
    uint32_t u32() {
        take(4);
        const size_t s = pos_ - 4;
        return (uint32_t(data_[s]) << 24) | (uint32_t(data_[s + 1]) << 16)
             | (uint32_t(data_[s + 2]) << 8) | uint32_t(data_[s + 3]);
    }
    uint64_t u64() {
        take(8);
        const size_t s = pos_ - 8;
        uint64_t v = 0;
        for (size_t i = 0; i < 8; ++i) v = (v << 8) | data_[s + i];
        return v;
    }
    int8_t i8()   { return int8_t(u8()); }
    int16_t i16() { return int16_t(u16()); }
    int32_t i32() { return int32_t(u32()); }
    int64_t i64() { return int64_t(u64()); }
    float f32() {
        uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    double f64() {
        uint64_t bits = u64();
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    bool boolean() { return u8() != 0; }

    std::string string() {
        const uint16_t n = u16();
        if (n < 1) {
            throw MalformedMessage("string length must be >= 1");
        }
        const size_t content_begin = pos_;
        byte_range(n - 1);              // content
        if (u8() != 0) {                // NUL terminator
            throw MalformedMessage("string missing NUL terminator");
        }
        return std::string(reinterpret_cast<const char *>(data_.data() + content_begin),
                           n - 1);
    }
    std::vector<uint8_t> bytes() {
        const uint16_t n = u16();
        return byte_range(n);
    }

    template <typename Fn>
    void struct_(Fn fn) { fn(*this); }

private:
    void take(size_t n) {
        if (pos_ + n > data_.size()) {
            throw MalformedMessage("short read at " + std::to_string(pos_) +
                                   ": need " + std::to_string(n) + ", have " +
                                   std::to_string(data_.size() - pos_));
        }
        start_ = pos_;
        pos_ += n;
    }
    std::vector<uint8_t> byte_range(size_t n) {
        take(n);
        std::vector<uint8_t> out(data_.begin() + start_, data_.begin() + pos_);
        return out;
    }

    const std::vector<uint8_t> data_;
    size_t pos_ = 0;
    size_t start_ = 0;
};

} // namespace someip2

#endif // SOMEIP2_SER_HPP
// C++ v2 wire layer tests + byte-level interop checks.
#include "someip2/wire.hpp"
#include "someip2/ser.hpp"

#include <cstdio>
#include <cstring>
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
        catch (const someip2::MalformedMessage &) { threw = true; }       \
        CHECK(threw);                                                     \
    } while (0)

static const unsigned char GOLDEN[] = {
    0x12, 0x34, 0x00, 0x02, 0x00, 0x00, 0x00, 0x10,
    0x11, 0x11, 0x22, 0x22, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
};

static void test_wire_roundtrip() {
    someip2::Header h;
    h.service_id = 0x1234;
    h.method_id = 0x0002;
    h.client_id = 0x1111;
    h.session_id = 0x2222;
    std::vector<uint8_t> payload = {0, 0, 0, 3, 0, 0, 0, 4};

    someip2::Message m;
    m.header = h;
    m.payload = payload;
    std::vector<uint8_t> raw;
    m.to_bytes(raw);

    CHECK(raw.size() == sizeof(GOLDEN));
    CHECK(std::memcmp(raw.data(), GOLDEN, sizeof(GOLDEN)) == 0);

    someip2::Message parsed = someip2::Message::from_bytes(GOLDEN, sizeof(GOLDEN));
    CHECK(parsed.header.service_id == 0x1234);
    CHECK(parsed.header.method_id == 0x0002);
    CHECK(parsed.header.message_id() == 0x12340002);
    CHECK(parsed.header.request_id() == 0x11112222);
    CHECK(parsed.payload.size() == 8);
    CHECK(std::memcmp(parsed.payload.data(), payload.data(), 8) == 0);
}

static void test_wire_validation() {
    CHECK_THROWS(someip2::Message::from_bytes(GOLDEN, 8));      // short
    CHECK_THROWS(someip2::Message::decode_header(GOLDEN, 4));    // very short

    std::vector<uint8_t> truncated(GOLDEN, GOLDEN + 20);        // partial payload
    CHECK_THROWS(someip2::Message::from_bytes(truncated.data(), truncated.size()));

    // length field < 8 must be rejected.
    std::vector<uint8_t> bad(GOLDEN, GOLDEN + sizeof(GOLDEN));
    bad[4] = 0; bad[5] = 0; bad[6] = 0; bad[7] = 4;
    CHECK_THROWS(someip2::Message::from_bytes(bad.data(), bad.size()));
}

static void test_ser_roundtrip() {
    someip2::Writer w;
    w.u8(0xAB).u16(0x1234).u32(0xDEADBEEF).u64(0x0102030405060708ull);
    w.i8(-1).i16(-2).i32(-3).i64(-4);
    w.f32(1.5f).f64(-2.25).boolean(true).boolean(false);
    w.string("hello");
    const uint8_t blob[] = {1, 2, 3};
    w.bytes(blob, 3);

    someip2::Reader r(w.data());
    CHECK(r.u8() == 0xAB);
    CHECK(r.u16() == 0x1234);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0102030405060708ull);
    CHECK(r.i8() == -1);
    CHECK(r.i16() == -2);
    CHECK(r.i32() == -3);
    CHECK(r.i64() == -4);
    CHECK(std::fabs(r.f32() - 1.5f) < 1e-5);
    CHECK(std::fabs(r.f64() + 2.25) < 1e-9);
    CHECK(r.boolean() == true);
    CHECK(r.boolean() == false);
    CHECK(r.string() == "hello");
    std::vector<uint8_t> got = r.bytes();
    CHECK(got.size() == 3 && got[0] == 1 && got[2] == 3);
    CHECK(r.remaining() == 0);
}

static void test_ser_structure() {
    someip2::Writer w;
    w.struct_([&](someip2::Writer &inner) {
        inner.u8(7).string("tag");
    });
    someip2::Reader rw(w.data());
    rw.struct_([&](someip2::Reader &inner) {
        CHECK(inner.u8() == 7);
        CHECK(inner.string() == "tag");
    });
    CHECK(rw.remaining() == 0);

    // short read must throw
    someip2::Writer short_w;
    short_w.u16(1);
    someip2::Reader short_r(short_w.data());
    short_r.u16();
    CHECK_THROWS(short_r.u32());
}

static void test_v1_interop() {
    // Build bytes with the v1 layout helpers and parse them with v2 wire.
    std::vector<uint8_t> v1;
    v1.push_back(0x12); v1.push_back(0x34); v1.push_back(0x80); v1.push_back(0x01);
    uint32_t len = 8 + 8;
    v1.push_back(uint8_t(len >> 24)); v1.push_back(uint8_t(len >> 16));
    v1.push_back(uint8_t(len >> 8));  v1.push_back(uint8_t(len));
    v1.push_back(0x00); v1.push_back(0x00); v1.push_back(0x00); v1.push_back(0x01);
    v1.push_back(0x01); v1.push_back(0x01); v1.push_back(0x02); v1.push_back(0x00);
    v1.insert(v1.end(), {0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04});

    someip2::Message m = someip2::Message::from_bytes(v1.data(), v1.size());
    CHECK(m.header.service_id == 0x1234);
    CHECK(m.header.method_id == 0x8001);
    CHECK(m.header.message_type == 0x02);
    CHECK(m.header.is_notification());
    CHECK(m.payload.size() == 8);
}

int main() {
    test_wire_roundtrip();
    test_wire_validation();
    test_ser_roundtrip();
    test_ser_structure();
    test_v1_interop();
    if (failures == 0) {
        std::printf("C++ v2 wire/ser: ALL PASSED\n");
        return 0;
    }
    std::printf("C++ v2 wire/ser: %d failure(s)\n", failures);
    return 1;
}
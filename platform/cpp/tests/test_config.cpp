// C++ JSON parser + AppConfig tests (P1 config system).
#include "someip/config.hpp"

#include <cstdio>
#include <fstream>
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
        bool _threw = false;                                              \
        try {                                                             \
            (void)(expr);                                                 \
        } catch (const someip::JsonError &) {                             \
            _threw = true;                                                \
        }                                                                 \
        if (!_threw) {                                                    \
            std::printf("FAIL %s:%d: expected throw\n", __FILE__,         \
                        __LINE__);                                        \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static void test_json_parser() {
    const someip::JsonValue v = someip::JsonValue::parse(
        R"({"a": 1, "b": [1.5, "x", true, null], "c": {"d": "e"}})");
    CHECK(v.type() == someip::JsonValue::OBJECT);
    CHECK(v.as_object().at("a").as_int() == 1);
    const auto &arr = v.as_object().at("b").as_array();
    CHECK(arr.size() == 4);
    CHECK(arr[0].as_number() == 1.5);
    CHECK(arr[1].as_string() == "x");
    CHECK(arr[2].as_bool() == true);
    CHECK(arr[3].is_null());
    CHECK(v.as_object().at("c").as_object().at("d").as_string() == "e");

    const someip::JsonValue esc = someip::JsonValue::parse(
        R"({"s": "a\n\u00e9\ud83d\ude00"})");
    CHECK(esc.as_object().at("s").as_string() == "a\n\xc3\xa9\xf0\x9f\x98\x80");

    CHECK_THROWS(someip::JsonValue::parse(R"({"a": })"));
    CHECK_THROWS(someip::JsonValue::parse(R"({"a": 1,})"));
    CHECK_THROWS(someip::JsonValue::parse("[] extra"));
    CHECK_THROWS(someip::JsonValue::parse(""));
}

static void write_tmp(const std::string &path, const std::string &body) {
    std::ofstream f(path, std::ios::trunc);
    f << body;
}

static void test_app_config() {
    const std::string p = "/tmp/someip_cfg_test.json";
    write_tmp(p, R"({
        "unicast": "10.0.0.9",
        "sd": {"port": 31000, "multicast": "224.244.224.245", "ttl": 5},
        "service": {
            "service_id": "0x1A2B", "instance_id": 7,
            "major": 2, "minor": 0,
            "method_port": 32100, "event_port": 32101
        },
        "client": {"client_id": "0x0701", "sd_port": 31001,
                   "interface": "10.0.0.9"},
        "some_future_key": {"a": 1}
    })");
    someip::AppConfig cfg(p);
    CHECK(cfg.unicast == "10.0.0.9");
    CHECK(cfg.sd.port == 31000);
    CHECK(cfg.sd.ttl == 5);
    CHECK(cfg.service.present);
    CHECK(cfg.service.service_id == 0x1A2B);
    CHECK(cfg.service.instance_id == 7);
    CHECK(cfg.service.major == 2);
    CHECK(cfg.service.minor == 0);
    CHECK(cfg.service.method_port == 32100);
    CHECK(cfg.service.interface == "10.0.0.9");
    CHECK(cfg.client.present);
    CHECK(cfg.client.client_id == 0x0701);
    CHECK(cfg.client.sd_port == 31001);
    CHECK(cfg.client.interface == "10.0.0.9");
}

static void test_app_config_auto_and_defaults() {
    const std::string p = "/tmp/someip_cfg_auto.json";
    write_tmp(p, R"({"unicast": "auto", "service": {"service_id": "0x1234",
        "instance_id": "0x5678"}, "client": {"client_id": "auto"}})");
    someip::AppConfig cfg(p);
    CHECK(cfg.unicast.empty());

    cfg.load(p);
    someip::AppConfig d;
    (void)d;
}

static void test_app_config_errors() {
    const std::string p = "/tmp/someip_cfg_bad.json";
    write_tmp(p, R"({"sd": {"port": "nope"}})");
    CHECK_THROWS(someip::AppConfig(p));
    write_tmp(p, R"({"sd": {"port": -1}})");
    CHECK_THROWS(someip::AppConfig(p));
    write_tmp(p, R"(garbage)");
    CHECK_THROWS(someip::AppConfig(p));
    CHECK_THROWS(someip::AppConfig("/nonexistent/nope.json"));
}

int main() {
    test_json_parser();
    test_app_config();
    test_app_config_auto_and_defaults();
    test_app_config_errors();
    if (failures == 0) {
        std::printf("C++ v2 config: ALL PASSED\n");
        return 0;
    }
    std::printf("C++ v2 config: %d failure(s)\n", failures);
    return 1;
}
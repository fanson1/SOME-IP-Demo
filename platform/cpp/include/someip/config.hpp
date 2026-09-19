// Minimal JSON parser + vsomeip-schema-subset config (P1).
//
// Zero-dependency header-only implementation of just enough of RFC 8259 for
// config files: objects, arrays, strings, numbers, true/false/null with
// surrogate-pair escape handling. `AppConfig::load` then maps the vsomeip
// subset (unicast / sd / service / client) used by app.hpp and the demos.
#ifndef SOMEIP_CONFIG_HPP
#define SOMEIP_CONFIG_HPP

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace someip {

struct JsonError : std::runtime_error {
    explicit JsonError(const std::string &what) : std::runtime_error(what) {}
};

// ------------------------------------------------------------------ JsonValue

class JsonValue {
public:
    enum Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    JsonValue() : type_(NUL) {}
    static JsonValue make_bool(bool v) { JsonValue j; j.bool_ = v; j.type_ = BOOL; return j; }
    static JsonValue make_number(double v) { JsonValue j; j.number_ = v; j.type_ = NUMBER; return j; }
    static JsonValue make_string(std::string v) { JsonValue j; j.string_ = std::move(v); j.type_ = STRING; return j; }
    static JsonValue make_array(std::vector<JsonValue> v) { JsonValue j; j.array_ = std::move(v); j.type_ = ARRAY; return j; }
    static JsonValue make_object(std::map<std::string, JsonValue> v) { JsonValue j; j.object_ = std::move(v); j.type_ = OBJECT; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == NUL; }
    bool as_bool() const {
        if (type_ != BOOL) throw JsonError("not a bool");
        return bool_;
    }
    double as_number() const {
        if (type_ != NUMBER) throw JsonError("not a number");
        return number_;
    }
    long long as_int() const { return static_cast<long long>(as_number()); }
    const std::string &as_string() const {
        if (type_ != STRING) throw JsonError("not a string");
        return string_;
    }
    const std::vector<JsonValue> &as_array() const {
        if (type_ != ARRAY) throw JsonError("not an array");
        return array_;
    }
    const std::map<std::string, JsonValue> &as_object() const {
        if (type_ != OBJECT) throw JsonError("not an object");
        return object_;
    }
    const JsonValue *find(const std::string &key) const {
        if (type_ != OBJECT) return nullptr;
        auto it = object_.find(key);
        return it == object_.end() ? nullptr : &it->second;
    }

    static JsonValue parse(const std::string &text);

private:
    Type type_;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::map<std::string, JsonValue> object_;
};

namespace detail {
struct JsonParser {
    const char *p;
    const char *end;
    explicit JsonParser(const std::string &text) : p(text.data()), end(text.data() + text.size()) {}

    void ws() {
        while (p < end && (unsigned char)*p <= ' ') ++p;
    }
    [[noreturn]] void fail(const std::string &what) const { throw JsonError(what); }
    bool eat(char c) {
        if (p < end && *p == c) { ++p; return true; }
        return false;
    }
    void expect(char c) {
        if (!eat(c)) fail(std::string("expected '") + c + "'");
    }

    JsonValue value() {
        ws();
        if (p == end) fail("unexpected end");
        switch (*p) {
            case '{': return object();
            case '[': return array();
            case '"': return JsonValue::make_string(string());
            case 't': expect_keyword("true"); return JsonValue::make_bool(true);
            case 'f': expect_keyword("false"); return JsonValue::make_bool(false);
            case 'n': expect_keyword("null"); return JsonValue();
            default:
                if (*p == '-' || *p == '+' || (*p >= '0' && *p <= '9')) return number();
                fail("unexpected character");
        }
    }
    void expect_keyword(const char *kw) {
        for (const char *k = kw; *k; ++k, ++p) {
            if (*p != *k) fail("bad literal");
        }
    }
    JsonValue number() {
        const char *start = p;
        eat('-');
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (eat('.')) while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        return JsonValue::make_number(std::strtod(std::string(start, p).c_str(), nullptr));
    }
    std::string string() {
        expect('"');
        std::string out;
        while (true) {
            if (p == end) fail("unterminated string");
            char c = *p++;
            if (c == '"') return out;
            if (c == '\\') {
                if (p == end) fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': out += unicode_escape(); break;
                    default: fail("bad escape");
                }
            } else {
                out += c;
            }
        }
    }
    uint32_t hex4() {
        if (end - p < 4) fail("short \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            char c = *p;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }
    std::string unicode_escape() {
        uint32_t cp = hex4();
        if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: expect low pair
            if (end - p >= 2 && p[0] == '\\' && p[1] == 'u') {
                p += 2;
                uint32_t lo = hex4();
                if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
        }
        std::string out;
        if (cp <= 0x7F) {
            out += char(cp);
        } else if (cp <= 0x7FF) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
        return out;
    }
    JsonValue object() {
        expect('{');
        std::map<std::string, JsonValue> out;
        ws();
        if (eat('}')) return JsonValue::make_object(std::move(out));
        while (true) {
            ws();
            std::string key = string();
            ws();
            expect(':');
            out.emplace(std::move(key), value());
            ws();
            if (eat('}')) return JsonValue::make_object(std::move(out));
            expect(',');
        }
    }
    JsonValue array() {
        expect('[');
        std::vector<JsonValue> out;
        ws();
        if (eat(']')) return JsonValue::make_array(std::move(out));
        while (true) {
            out.push_back(value());
            ws();
            if (eat(']')) return JsonValue::make_array(std::move(out));
            expect(',');
        }
    }
};
}  // namespace detail

inline JsonValue JsonValue::parse(const std::string &text) {
    detail::JsonParser parser(text);
    JsonValue v = parser.value();
    parser.ws();
    if (parser.p != parser.end) {
        throw JsonError("trailing characters after JSON value");
    }
    return v;
}

// --------------------------------------------------------------- AppConfig

struct AppConfig {
    struct Sd {
        uint16_t port = 30490;
        std::string multicast = "224.244.224.245";
        uint16_t ttl = 3;
    } sd;
    struct Service {
        bool present = false;
        uint32_t service_id = 0x1234;
        uint32_t instance_id = 0x5678;
        uint8_t major = 1;
        uint32_t minor = 1;
        uint16_t method_port = 30500;
        uint16_t event_port = 30501;
        std::string interface;  // empty == auto
    } service;
    struct Client {
        bool present = false;
        uint32_t client_id = 0;  // 0 == auto
        uint16_t sd_port = 30490;
        std::string interface;   // empty == auto
    } client;
    std::string unicast;         // empty == auto
    std::string path;
    std::string log_level;       // empty == default (info / env)

    static uint32_t euid(const std::string &s) {
        errno = 0;
        char *endp = nullptr;
        const long long v = std::strtoll(s.c_str(), &endp, 0);
        if (endp == s.c_str() || v < 0 || v > 0xFFFFFFFFLL) {
            throw JsonError("bad integer literal: " + s);
        }
        return uint32_t(v);
    }
    static uint16_t u16(const char *what, long long v) {
        if (v < 0 || v > 0xFFFF) throw JsonError(std::string(what) + " out of uint16 range");
        return uint16_t(v);
    }
    static uint32_t int_or_hex(const JsonValue &v, const char *what, uint32_t fallback) {
        if (v.is_null()) return fallback;
        if (v.type() == JsonValue::NUMBER) return u16(what, v.as_int());
        return euid(v.as_string());
    }
    static std::string iface(const std::string &s) { return (s.empty() || s == "auto") ? std::string() : s; }

    AppConfig() = default;
    explicit AppConfig(const std::string &config_path) { load(config_path); }

    bool load(const std::string &config_path) {
        std::ifstream f(config_path);
        if (!f) {
            throw JsonError("cannot open config file: " + config_path);
        }
        std::stringstream ss;
        ss << f.rdbuf();
        const JsonValue root = JsonValue::parse(ss.str());
        if (root.type() != JsonValue::OBJECT) {
            throw JsonError("config root must be an object");
        }
        path = config_path;
        if (const JsonValue *v = root.find("unicast")) {
            unicast = iface(v->as_string());
        }
        if (const JsonValue *v = root.find("sd")) {
            const auto &o = v->as_object();
            if (const JsonValue *x = member(o, "port")) sd.port = u16("sd.port", x->as_int());
            if (const JsonValue *x = member(o, "multicast")) sd.multicast = x->as_string();
            if (const JsonValue *x = member(o, "ttl")) sd.ttl = u16("sd.ttl", x->as_int());
        }
        if (const JsonValue *v = root.find("log")) {
            const auto &o = v->as_object();
            if (const JsonValue *x = member(o, "level")) log_level = x->as_string();
        }
        if (const JsonValue *v = root.find("service")) {
            const auto &o = v->as_object();
            service.present = true;
            if (const JsonValue *x = member(o, "service_id")) service.service_id = int_or_hex(*x, "service.service_id", service.service_id);
            if (const JsonValue *x = member(o, "instance_id")) service.instance_id = int_or_hex(*x, "service.instance_id", service.instance_id);
            if (const JsonValue *x = member(o, "major")) service.major = uint8_t(u16("service.major", x->as_int()));
            if (const JsonValue *x = member(o, "minor")) service.minor = u16("service.minor", x->as_int());
            if (const JsonValue *x = member(o, "major_version")) service.major = uint8_t(u16("service.major_version", x->as_int()));
            if (const JsonValue *x = member(o, "minor_version")) service.minor = u16("service.minor_version", x->as_int());
            if (const JsonValue *x = member(o, "method_port")) service.method_port = u16("service.method_port", x->as_int());
            if (const JsonValue *x = member(o, "event_port")) service.event_port = u16("service.event_port", x->as_int());
            if (const JsonValue *x = member(o, "interface")) service.interface = iface(x->as_string());
            if (service.interface.empty()) service.interface = iface(unicast);
        }
        if (const JsonValue *v = root.find("client")) {
            const auto &o = v->as_object();
            client.present = true;
            if (const JsonValue *x = member(o, "client_id")) {
                if (x->type() == JsonValue::STRING && x->as_string() == "auto") {
                    client.client_id = 0;
                } else {
                    client.client_id = int_or_hex(*x, "client.client_id", 0);
                }
            }
            if (const JsonValue *x = member(o, "sd_port")) client.sd_port = u16("client.sd_port", x->as_int());
            if (const JsonValue *x = member(o, "interface")) client.interface = iface(x->as_string());
        }
        return true;
    }

private:
    static const JsonValue *member(const std::map<std::string, JsonValue> &o,
                                   const std::string &key) {
        auto it = o.find(key);
        return it == o.end() ? nullptr : &it->second;
    }
};

}  // namespace someip

#endif  // SOMEIP_CONFIG_HPP
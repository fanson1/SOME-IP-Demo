// vsomeip service: offers a SOME/IP service with Method / Field / Event,
// mirroring the service definition used by the Python and hand-written C++
// demos (Service 0x1234/0x5678, EventGroup 0x0001).
#include <vsomeip/vsomeip.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static const vsomeip::service_t   SERVICE_ID        = 0x1234;
static const vsomeip::instance_t  INSTANCE_ID       = 0x5678;
static const vsomeip::method_t    METHOD_GET_VERSION= 0x0001;
static const vsomeip::method_t    METHOD_ADD        = 0x0002;
static const vsomeip::event_t     FIELD_SPEED       = 0x1000;
static const vsomeip::event_t     EVENT_STATUS      = 0x8001;
static const vsomeip::eventgroup_t EVENTGROUP_MAIN  = 0x0001;

static std::shared_ptr<vsomeip::application> g_app;
static std::atomic<uint32_t> g_speed{0};

static std::shared_ptr<vsomeip::payload> make_u32_payload(uint32_t value) {
    auto payload = vsomeip::runtime::get()->create_payload();
    std::vector<vsomeip::byte_t> bytes{
        static_cast<vsomeip::byte_t>(value >> 24),
        static_cast<vsomeip::byte_t>(value >> 16),
        static_cast<vsomeip::byte_t>(value >> 8),
        static_cast<vsomeip::byte_t>(value)};
    payload->set_data(bytes);
    return payload;
}

static bool read_u32(const vsomeip::byte_t *data, size_t length,
                     size_t offset, uint32_t &out) {
    if (offset + 4 > length) {
        return false;
    }
    out = static_cast<uint32_t>(data[offset]) << 24 |
          static_cast<uint32_t>(data[offset + 1]) << 16 |
          static_cast<uint32_t>(data[offset + 2]) << 8 |
          static_cast<uint32_t>(data[offset + 3]);
    return true;
}

static void publish_speed() {
    uint32_t value = g_speed.load();
    g_app->notify(SERVICE_ID, INSTANCE_ID, FIELD_SPEED + 2, make_u32_payload(value));
}

static void publish_status() {
    auto payload = vsomeip::runtime::get()->create_payload();
    std::vector<vsomeip::byte_t> raw;
    uint32_t ts = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
    raw.push_back(static_cast<vsomeip::byte_t>(ts >> 24));
    raw.push_back(static_cast<vsomeip::byte_t>(ts >> 16));
    raw.push_back(static_cast<vsomeip::byte_t>(ts >> 8));
    raw.push_back(static_cast<vsomeip::byte_t>(ts));
    g_speed = (g_speed + 10) % 220;
    uint32_t speed = g_speed.load();
    raw.push_back(static_cast<vsomeip::byte_t>(speed >> 24));
    raw.push_back(static_cast<vsomeip::byte_t>(speed >> 16));
    raw.push_back(static_cast<vsomeip::byte_t>(speed >> 8));
    raw.push_back(static_cast<vsomeip::byte_t>(speed));
    payload->set_data(raw);
    g_app->notify(SERVICE_ID, INSTANCE_ID, EVENT_STATUS, payload);
}

static void on_message(const std::shared_ptr<vsomeip::message> &request) {
    vsomeip::method_t method = request->get_method();
    std::shared_ptr<vsomeip::payload> response_payload;
    vsomeip::return_code_e rc = vsomeip::return_code_e::E_OK;

    const vsomeip::byte_t *data = request->get_payload()->get_data();
    size_t length = request->get_payload()->get_length();

    switch (method) {
        case METHOD_GET_VERSION:
            response_payload = make_u32_payload(0x01000000u);
            break;
        case METHOD_ADD: {
            uint32_t a = 0, b = 0;
            if (!read_u32(data, length, 0, a) || !read_u32(data, length, 4, b)) {
                rc = vsomeip::return_code_e::E_MALFORMED_MESSAGE;
                break;
            }
            response_payload = make_u32_payload(a + b);
            break;
        }
        case FIELD_SPEED:
            response_payload = make_u32_payload(g_speed.load());
            break;
        case FIELD_SPEED + 1: {
            uint32_t value = 0;
            if (!read_u32(data, length, 0, value)) {
                rc = vsomeip::return_code_e::E_MALFORMED_MESSAGE;
                break;
            }
            g_speed = value;
            response_payload = make_u32_payload(g_speed.load());
            publish_speed();
            break;
        }
        default:
            rc = vsomeip::return_code_e::E_UNKNOWN_METHOD;
            break;
    }

    if (response_payload == nullptr) {
        response_payload = vsomeip::runtime::get()->create_payload();
    }
    auto response = vsomeip::runtime::get()->create_response(request);
    response->set_return_code(rc);
    response->set_payload(response_payload);
    g_app->send(response);
}

int main() {
    g_app = vsomeip::runtime::get()->create_application("someip-service");
    if (!g_app->init()) {
        fprintf(stderr, "vsomeip init failed\n");
        return 1;
    }
    g_app->register_message_handler(SERVICE_ID, INSTANCE_ID, METHOD_GET_VERSION, on_message);
    g_app->register_message_handler(SERVICE_ID, INSTANCE_ID, METHOD_ADD, on_message);
    g_app->register_message_handler(SERVICE_ID, INSTANCE_ID, FIELD_SPEED, on_message);
    g_app->register_message_handler(SERVICE_ID, INSTANCE_ID, FIELD_SPEED + 1, on_message);

    g_app->offer_event(SERVICE_ID, INSTANCE_ID, EVENT_STATUS,
                       {EVENTGROUP_MAIN}, vsomeip::event_type_e::ET_EVENT);
    g_app->offer_event(SERVICE_ID, INSTANCE_ID, FIELD_SPEED + 2,
                       {EVENTGROUP_MAIN}, vsomeip::event_type_e::ET_EVENT);
    g_app->offer_service(SERVICE_ID, INSTANCE_ID);

    printf("vsomeip service: 0x%04X/0x%04X offered\n", SERVICE_ID, INSTANCE_ID);
    std::thread publisher([] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            publish_status();
            publish_speed();
        }
    });
    publisher.detach();

    g_app->start();
    return 0;
}
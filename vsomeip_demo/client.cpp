// vsomeip client: discovers the service via SD, calls Method / Field RPCs
// and receives Event / Field notifications. Events arrive via the generic
// message handler (vsomeip >= 3.7 dropped register_event_handler).
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
static std::atomic<bool> g_available{false};

static void send_request(vsomeip::method_t method,
                         const std::vector<uint32_t> &args = {}) {
    auto request = vsomeip::runtime::get()->create_request();
    request->set_service(SERVICE_ID);
    request->set_instance(INSTANCE_ID);
    request->set_method(method);
    if (!args.empty()) {
        auto payload = vsomeip::runtime::get()->create_payload();
        std::vector<vsomeip::byte_t> bytes;
        for (uint32_t value : args) {
            bytes.push_back(static_cast<vsomeip::byte_t>(value >> 24));
            bytes.push_back(static_cast<vsomeip::byte_t>(value >> 16));
            bytes.push_back(static_cast<vsomeip::byte_t>(value >> 8));
            bytes.push_back(static_cast<vsomeip::byte_t>(value));
        }
        payload->set_data(bytes);
        request->set_payload(payload);
    }
    g_app->send(request);
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

static void on_response(const std::shared_ptr<vsomeip::message> &response) {
    vsomeip::method_t method = response->get_method();
    const char *name = "method";
    uint32_t result = 0;
    if (method == METHOD_GET_VERSION) name = "GetVersion";
    else if (method == METHOD_ADD) name = "Add";
    else if (method == FIELD_SPEED) name = "ReadSpeed";
    else if (method == FIELD_SPEED + 1) name = "WriteSpeed";
    read_u32(response->get_payload()->get_data(),
             response->get_payload()->get_length(), 0, result);
    printf("  [resp 0x%04X] %-10s rc=0x%02X value=%u\n",
           method, name, static_cast<unsigned int>(response->get_return_code()), result);
}

static void on_event(const std::shared_ptr<vsomeip::message> &notification) {
    vsomeip::method_t event = notification->get_method();
    const vsomeip::byte_t *data = notification->get_payload()->get_data();
    size_t length = notification->get_payload()->get_length();
    uint32_t a = 0, b = 0;
    read_u32(data, length, 0, a);
    read_u32(data, length, 4, b);
    if (event == EVENT_STATUS) {
        printf("  [event 0x%04X] status: ts=%u speed=%u km/h\n", event, a, b);
    } else if (event == FIELD_SPEED + 2) {
        printf("  [notify 0x%04X] speed field = %u km/h\n", event, a);
    } else {
        printf("  [event 0x%04X] %zu bytes\n", event, length);
    }
}

static void on_any_message(const std::shared_ptr<vsomeip::message> &message) {
    if (message->get_message_type() == vsomeip::message_type_e::MT_NOTIFICATION) {
        on_event(message);
    } else {
        on_response(message);
    }
}

static void on_availability(vsomeip::service_t service, vsomeip::instance_t instance,
                            bool available) {
    if (available && !g_available.exchange(true)) {
        printf("Service 0x%04X/0x%04X is available\n", service, instance);
        g_app->request_event(SERVICE_ID, INSTANCE_ID, EVENT_STATUS,
                             {EVENTGROUP_MAIN}, vsomeip::event_type_e::ET_EVENT);
        g_app->request_event(SERVICE_ID, INSTANCE_ID, FIELD_SPEED + 2,
                             {EVENTGROUP_MAIN}, vsomeip::event_type_e::ET_EVENT);
        g_app->subscribe(SERVICE_ID, INSTANCE_ID, EVENTGROUP_MAIN);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        send_request(METHOD_GET_VERSION);
        send_request(METHOD_ADD, {3, 4});
        send_request(FIELD_SPEED);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        send_request(FIELD_SPEED + 1, {88});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        send_request(FIELD_SPEED);
    }
}

int main() {
    g_app = vsomeip::runtime::get()->create_application("someip-client");
    if (!g_app->init()) {
        fprintf(stderr, "vsomeip init failed\n");
        return 1;
    }
    g_app->register_message_handler(vsomeip::ANY_SERVICE, INSTANCE_ID,
                                    vsomeip::ANY_METHOD, on_any_message);
    g_app->register_availability_handler(SERVICE_ID, INSTANCE_ID, on_availability);
    g_app->request_service(SERVICE_ID, INSTANCE_ID);

    g_app->start();
    return 0;
}
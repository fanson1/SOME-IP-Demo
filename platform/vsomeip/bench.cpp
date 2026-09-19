// vsomeip RPC latency/throughput benchmark (P2 parity with benchmarks/bench_rpc.py).
//
// Strictly sequential round trips: exactly one request in flight, we wait for
// the next REQUEST response before sending again, no session correlation
// required (the response handler only flips a flag). Mirrors the methodology of
// bencchmarks/bench_rpc.py (send, wait for the single outstanding reply, time
// it) so the numbers are comparable. Prints a stable machine-readable block:
//   rtt_avg_ms=... rtt_p50_ms=... rtt_p90_ms=... req_per_s=... completed=N/N
//   BENCH PASS
#include <vsomeip/vsomeip.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

static const vsomeip::service_t  SERVICE_ID  = 0x1234;
static const vsomeip::instance_t INSTANCE_ID = 0x5678;
static const vsomeip::method_t   METHOD_ADD  = 0x0002;
static const size_t N = 100;

static std::shared_ptr<vsomeip::application> g_app;
static std::atomic<bool> g_started{false};
static std::atomic<bool> g_ready{false};
static std::atomic<size_t> g_received_any{0};
static std::vector<double> g_rtt_ms;

static void on_response(const std::shared_ptr<vsomeip::message> &response) {
    g_received_any.fetch_add(1);
    if (response->get_method() == METHOD_ADD &&
        response->get_message_type() == vsomeip::message_type_e::MT_RESPONSE) {
        g_ready.store(true);
    }
}

static void run_bench() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto bench_start = std::chrono::steady_clock::now();
    size_t completed = 0;
    for (size_t i = 0; i < N; ++i) {
        auto request = vsomeip::runtime::get()->create_request();
        request->set_service(SERVICE_ID);
        request->set_instance(INSTANCE_ID);
        request->set_method(METHOD_ADD);
        std::vector<vsomeip::byte_t> bytes{0, 0, 0, 3, 0, 0, 0, 4};  // 3 + 4
        auto payload = vsomeip::runtime::get()->create_payload();
        payload->set_data(bytes);
        request->set_payload(payload);

        g_ready.store(false);
        const auto t0 = std::chrono::steady_clock::now();
        g_app->send(request);
        auto deadline = t0 + std::chrono::milliseconds(300);
        while (!g_ready.load()) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (g_ready.load()) {
            ++completed;
            g_rtt_ms.push_back(ms);
        }
    }
    const double wall_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - bench_start).count();
    const size_t received = g_received_any.load();

    if (completed != N) {
        std::printf("completed=%zu/%zu BENCH FAIL (received_any=%zu rtt_samples=%zu)\n",
                    completed, N, received, g_rtt_ms.size());
        g_app->stop();
        return;
    }

    std::vector<double> v = g_rtt_ms;
    std::sort(v.begin(), v.end());
    const double avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    const double p50 = v[v.size() / 2];
    const double p90 = v[size_t(v.size() * 0.9)];
    std::printf("rtt_avg_ms=%.3f rtt_p50_ms=%.3f rtt_p90_ms=%.3f "
                "req_per_s=%.1f\n", avg, p50, p90, 1000.0 / avg);
    std::printf("completed=%zu/%zu\n", v.size(), N);
    std::printf("benched_received_any=%zu wall_s=%.3f\n", received, wall_s);
    std::printf("BENCH PASS\n");
    std::fflush(stdout);
    g_app->stop();
}

static void on_availability(vsomeip::service_t service, vsomeip::instance_t instance,
                            bool available) {
    if (available && !g_started.exchange(true)) {
        std::printf("Service 0x%04X/0x%04X is available (bench)\n", service, instance);
        std::thread([] { run_bench(); }).detach();
    }
}

static void on_state(vsomeip::state_type_e state) {
    if (state == vsomeip::state_type_e::ST_REGISTERED) {
        g_app->request_service(SERVICE_ID, INSTANCE_ID);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_app = vsomeip::runtime::get()->create_application("someip-bench");
    if (!g_app->init()) {
        fprintf(stderr, "vsomeip init failed\n");
        return 1;
    }
    g_app->register_state_handler(on_state);
    g_app->register_availability_handler(SERVICE_ID, INSTANCE_ID, on_availability);
    g_app->register_message_handler(vsomeip::ANY_SERVICE, INSTANCE_ID,
                                    vsomeip::ANY_METHOD, on_response);
    g_app->start();
    return 0;
}